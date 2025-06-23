/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/Compression.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/MemoryChecking.h"
#include "mozilla/PodOperations.h"
#include "mozilla/ScopeExit.h"

#include "js/Prefs.h"
#include "js/Utility.h"
#include "util/Memory.h"

using namespace js;

static void* zlib_alloc(void* cx, uInt items, uInt size) {
  return js_calloc(items, size);
}

static void zlib_free(void* cx, void* addr) { js_free(addr); }

Compressor::Compressor(const unsigned char* inp, size_t inplen,
                       CompressionAlgorithm algorithm, uint8_t level)
    : algorithm_(algorithm),
      level_(level),
      inp(inp),
      inplen(inplen),
      initialized(false),
      finished(false),
      currentChunkSize(0),
      zstdInputPos(0) {
  MOZ_ASSERT(inplen > 0, "data to compress can't be empty");

  if (algorithm_ == CompressionAlgorithm::ZLIB) {
    zs.opaque = nullptr;
    zs.next_in = (Bytef*)inp;
    zs.avail_in = 0;
    zs.next_out = nullptr;
    zs.avail_out = 0;
    zs.zalloc = zlib_alloc;
    zs.zfree = zlib_free;
    zs.total_in = 0;
    zs.total_out = 0;
    zs.msg = nullptr;
    zs.state = nullptr;
    zs.data_type = 0;
    zs.adler = 0;
    zs.reserved = 0;
  } else {
    zstdCStream = nullptr;
  }

  // Reserve space for the CompressedDataHeader.
  outbytes = sizeof(CompressedDataHeader);
}

Compressor::~Compressor() {
  if (initialized) {
    if (algorithm_ == CompressionAlgorithm::ZLIB) {
      int ret = deflateEnd(&zs);
      if (ret != Z_OK) {
        // If we finished early, we can get a Z_DATA_ERROR.
        MOZ_ASSERT(ret == Z_DATA_ERROR);
        MOZ_ASSERT(!finished);
      }
    } else if (algorithm_ == CompressionAlgorithm::ZSTD) {
      if (zstdCStream) {
        ZSTD_freeCStream(zstdCStream);
      }
    }
  }
}

// According to the zlib docs, the default value for windowBits is 15. Passing
// -15 is treated the same, but it also forces 'raw deflate' (no zlib header or
// trailer). Raw deflate is necessary for chunked decompression.
static const int WindowBits = -15;

bool Compressor::init() {
  if (inplen >= UINT32_MAX) {
    return false;
  }

  if (algorithm_ == CompressionAlgorithm::ZLIB) {
    // Determine compression level
    int zlibLevel;
    if (level_ == 0) {
      // Use default based on build configuration
#ifdef USE_LIBZ_RS
      zlibLevel = 2;
#else
      zlibLevel = Z_BEST_SPEED;
#endif
    } else {
      // Use specified level (1-9 for zlib)
      zlibLevel = level_;
    }

    int ret = deflateInit2(&zs, zlibLevel, Z_DEFLATED, WindowBits, 8, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) {
      MOZ_ASSERT(ret == Z_MEM_ERROR);
      return false;
    }
  } else if (algorithm_ == CompressionAlgorithm::ZSTD) {
    zstdCStream = ZSTD_createCStream();
    if (!zstdCStream) {
      return false;
    }

    // Determine compression level (1-22 for zstd, default 3)
    int zstdLevel = (level_ == 0) ? 3 : level_;

    size_t ret = ZSTD_initCStream(zstdCStream, zstdLevel);
    if (ZSTD_isError(ret)) {
      ZSTD_freeCStream(zstdCStream);
      zstdCStream = nullptr;
      return false;
    }
  } else {
    return false;
  }

  initialized = true;
  return true;
}

void Compressor::setOutput(unsigned char* out, size_t outlen) {
  MOZ_ASSERT(outlen > outbytes);
  
  if (algorithm_ == CompressionAlgorithm::ZLIB) {
    zs.next_out = out + outbytes;
    zs.avail_out = outlen - outbytes;
  } else if (algorithm_ == CompressionAlgorithm::ZSTD) {
    zstdOutput.dst = out + outbytes;
    zstdOutput.size = outlen - outbytes;
    zstdOutput.pos = 0;
  }
}

Compressor::Status Compressor::compressMore() {
  if (algorithm_ == CompressionAlgorithm::ZLIB) {
    return compressMoreZlib();
  } else if (algorithm_ == CompressionAlgorithm::ZSTD) {
    return compressMoreZstd();
  }
  return OOM;
}

Compressor::Status Compressor::compressMoreZlib() {
  MOZ_ASSERT(zs.next_out);
  uInt left = inplen - (zs.next_in - inp);
  if (left <= MAX_INPUT_SIZE) {
    zs.avail_in = left;
  } else if (zs.avail_in == 0) {
    zs.avail_in = MAX_INPUT_SIZE;
  }

  // Finish the current chunk if needed.
  bool flush = false;
  MOZ_ASSERT(currentChunkSize <= CHUNK_SIZE);
  if (currentChunkSize + zs.avail_in >= CHUNK_SIZE) {
    // Adjust avail_in, so we don't get chunks that are larger than
    // CHUNK_SIZE.
    zs.avail_in = CHUNK_SIZE - currentChunkSize;
    MOZ_ASSERT(currentChunkSize + zs.avail_in == CHUNK_SIZE);
    flush = true;
  }

  MOZ_ASSERT(zs.avail_in <= left);
  bool done = zs.avail_in == left;

  Bytef* oldin = zs.next_in;
  Bytef* oldout = zs.next_out;
  int ret = deflate(&zs, done ? Z_FINISH : (flush ? Z_FULL_FLUSH : Z_NO_FLUSH));
  outbytes += zs.next_out - oldout;
  currentChunkSize += zs.next_in - oldin;
  MOZ_ASSERT(currentChunkSize <= CHUNK_SIZE);

  if (ret == Z_MEM_ERROR) {
    zs.avail_out = 0;
    return OOM;
  }
  if (ret == Z_BUF_ERROR || (ret == Z_OK && zs.avail_out == 0)) {
    // We have to resize the output buffer. Note that we're not done yet
    // because ret != Z_STREAM_END.
    MOZ_ASSERT(zs.avail_out == 0);
    return MOREOUTPUT;
  }

  if (done || currentChunkSize == CHUNK_SIZE) {
    MOZ_ASSERT_IF(!done, flush);
    MOZ_ASSERT(chunkSize(inplen, chunkOffsets.length()) == currentChunkSize);
    if (!chunkOffsets.append(outbytes)) {
      return OOM;
    }
    currentChunkSize = 0;
    MOZ_ASSERT_IF(done, chunkOffsets.length() == (inplen - 1) / CHUNK_SIZE + 1);
  }

  MOZ_ASSERT_IF(!done, ret == Z_OK);
  MOZ_ASSERT_IF(done, ret == Z_STREAM_END);
  return done ? DONE : CONTINUE;
}

Compressor::Status Compressor::compressMoreZstd() {
  size_t left = inplen - zstdInputPos;
  size_t inputSize;
  if (left <= MAX_INPUT_SIZE) {
    inputSize = left;
  } else {
    inputSize = MAX_INPUT_SIZE;
  }

  // Finish the current chunk if needed.
  bool flush = false;
  MOZ_ASSERT(currentChunkSize <= CHUNK_SIZE);
  if (currentChunkSize + inputSize >= CHUNK_SIZE) {
    // Adjust inputSize, so we don't get chunks that are larger than
    // CHUNK_SIZE.
    inputSize = CHUNK_SIZE - currentChunkSize;
    MOZ_ASSERT(currentChunkSize + inputSize == CHUNK_SIZE);
    flush = true;
  }

  MOZ_ASSERT(inputSize <= left);
  bool done = inputSize == left;

  ZSTD_inBuffer zstdInput;
  zstdInput.src = inp + zstdInputPos;
  zstdInput.size = inputSize;
  zstdInput.pos = 0;

  size_t oldOutputPos = zstdOutput.pos;
  size_t ret;
  
  if (done) {
    // First compress any remaining input
    if (zstdInput.size > 0) {
      ret = ZSTD_compressStream(zstdCStream, &zstdOutput, &zstdInput);
      if (ZSTD_isError(ret)) {
        return OOM;
      }
    }
    // Then end the stream
    ret = ZSTD_endStream(zstdCStream, &zstdOutput);
  } else if (flush) {
    // First compress any remaining input
    if (zstdInput.size > 0) {
      ret = ZSTD_compressStream(zstdCStream, &zstdOutput, &zstdInput);
      if (ZSTD_isError(ret)) {
        return OOM;
      }
    }
    // Then flush the stream
    ret = ZSTD_flushStream(zstdCStream, &zstdOutput);
  } else {
    ret = ZSTD_compressStream(zstdCStream, &zstdOutput, &zstdInput);
  }

  if (ZSTD_isError(ret)) {
    return OOM;
  }

  outbytes += zstdOutput.pos - oldOutputPos;
  currentChunkSize += zstdInput.pos;
  zstdInputPos += zstdInput.pos;
  MOZ_ASSERT(currentChunkSize <= CHUNK_SIZE);

  // Check if we need more output space
  if (zstdOutput.pos == zstdOutput.size && !done) {
    return MOREOUTPUT;
  }

  if (done || currentChunkSize == CHUNK_SIZE) {
    MOZ_ASSERT_IF(!done, flush);
    MOZ_ASSERT(chunkSize(inplen, chunkOffsets.length()) == currentChunkSize);
    if (!chunkOffsets.append(outbytes)) {
      return OOM;
    }
    currentChunkSize = 0;
    MOZ_ASSERT_IF(done, chunkOffsets.length() == (inplen - 1) / CHUNK_SIZE + 1);
  }

  // For zstd, we're done when endStream returns 0
  if (done && ret == 0) {
    return DONE;
  }
  
  return done ? DONE : CONTINUE;
}

size_t Compressor::totalBytesNeeded() const {
  return AlignBytes(outbytes, sizeof(uint32_t)) + sizeOfChunkOffsets();
}

void Compressor::finish(char* dest, size_t destBytes) {
  MOZ_ASSERT(!chunkOffsets.empty());

  CompressedDataHeader* compressedHeader =
      reinterpret_cast<CompressedDataHeader*>(dest);
  compressedHeader->compressedBytes = outbytes;
  compressedHeader->algorithm = algorithm_;
  compressedHeader->level = level_;
  compressedHeader->reserved = 0;

  size_t outbytesAligned = AlignBytes(outbytes, sizeof(uint32_t));

  // Zero the padding bytes, the ImmutableStringsCache will hash them.
  mozilla::PodZero(dest + outbytes, outbytesAligned - outbytes);

  uint32_t* destArr = reinterpret_cast<uint32_t*>(dest + outbytesAligned);

  MOZ_ASSERT(uintptr_t(dest + destBytes) ==
             uintptr_t(destArr + chunkOffsets.length()));
  mozilla::PodCopy(destArr, chunkOffsets.begin(), chunkOffsets.length());

  finished = true;
}

bool js::DecompressString(const unsigned char* inp, size_t inplen,
                          unsigned char* out, size_t outlen) {
  MOZ_ASSERT(inplen <= UINT32_MAX);
  
  // Auto-detect algorithm from header
  if (inplen < sizeof(CompressedDataHeader)) {
    return false;
  }
  
  const CompressedDataHeader* header =
      reinterpret_cast<const CompressedDataHeader*>(inp);
  
  CompressionAlgorithm algorithm = header->algorithm;
  
  if (algorithm == CompressionAlgorithm::ZLIB) {
    // Mark the memory we pass to zlib as initialized for MSan.
    MOZ_MAKE_MEM_DEFINED(out, outlen);

    z_stream zs;
    zs.zalloc = zlib_alloc;
    zs.zfree = zlib_free;
    zs.opaque = nullptr;
    zs.next_in = (Bytef*)inp;
    zs.avail_in = inplen;
    zs.next_out = out;
    MOZ_ASSERT(outlen);
    zs.avail_out = outlen;
    int ret = inflateInit(&zs);
    if (ret != Z_OK) {
      MOZ_ASSERT(ret == Z_MEM_ERROR);
      return false;
    }
    ret = inflate(&zs, Z_FINISH);
    MOZ_ASSERT(ret == Z_STREAM_END);
    ret = inflateEnd(&zs);
    MOZ_ASSERT(ret == Z_OK);
    return true;
  } else if (algorithm == CompressionAlgorithm::ZSTD) {
    // Mark the memory for MSan.
    MOZ_MAKE_MEM_DEFINED(out, outlen);
    
    size_t result = ZSTD_decompress(out, outlen, inp, inplen);
    return !ZSTD_isError(result) && result == outlen;
  }
  
  return false;
}

bool js::DecompressStringChunk(const unsigned char* inp, size_t chunk,
                               unsigned char* out, size_t outlen) {
  MOZ_ASSERT(outlen <= Compressor::CHUNK_SIZE);

  const CompressedDataHeader* header =
      reinterpret_cast<const CompressedDataHeader*>(inp);

  CompressionAlgorithm algorithm = header->algorithm;
  size_t compressedBytes = header->compressedBytes;
  size_t compressedBytesAligned = AlignBytes(compressedBytes, sizeof(uint32_t));

  const unsigned char* offsetBytes = inp + compressedBytesAligned;
  const uint32_t* offsets = reinterpret_cast<const uint32_t*>(offsetBytes);

  uint32_t compressedStart =
      chunk > 0 ? offsets[chunk - 1] : sizeof(CompressedDataHeader);
  uint32_t compressedEnd = offsets[chunk];

  MOZ_ASSERT(compressedStart < compressedEnd);
  MOZ_ASSERT(compressedEnd <= compressedBytes);

  bool lastChunk = compressedEnd == compressedBytes;

  // Mark the memory as initialized for MSan.
  MOZ_MAKE_MEM_DEFINED(out, outlen);

  if (algorithm == CompressionAlgorithm::ZLIB) {
    z_stream zs;
    zs.zalloc = zlib_alloc;
    zs.zfree = zlib_free;
    zs.opaque = nullptr;
    zs.next_in = (Bytef*)(inp + compressedStart);
    zs.avail_in = compressedEnd - compressedStart;
    zs.next_out = out;
    MOZ_ASSERT(outlen);
    zs.avail_out = outlen;

    // Bug 1505857 - Use 'volatile' so variable is preserved in crashdump
    // when release-asserts below are tripped.
    volatile int ret = inflateInit2(&zs, WindowBits);
    if (ret != Z_OK) {
      MOZ_ASSERT(ret == Z_MEM_ERROR);
      return false;
    }

    auto autoCleanup = mozilla::MakeScopeExit([&] {
      mozilla::DebugOnly<int> ret = inflateEnd(&zs);
      MOZ_ASSERT(ret == Z_OK);
    });

    if (lastChunk) {
      ret = inflate(&zs, Z_FINISH);
      MOZ_RELEASE_ASSERT(ret == Z_STREAM_END);
    } else {
      ret = inflate(&zs, Z_NO_FLUSH);
      if (ret == Z_MEM_ERROR) {
        return false;
      }
      MOZ_RELEASE_ASSERT(ret == Z_OK);
    }
    MOZ_ASSERT(zs.avail_in == 0);
    MOZ_ASSERT(zs.avail_out == 0);
    return true;
  } else if (algorithm == CompressionAlgorithm::ZSTD) {
    // For zstd, decompress the chunk directly
    const unsigned char* chunkData = inp + compressedStart;
    size_t chunkSize = compressedEnd - compressedStart;
    
    size_t result = ZSTD_decompress(out, outlen, chunkData, chunkSize);
    return !ZSTD_isError(result) && result == outlen;
  }
  
  return false;
}
