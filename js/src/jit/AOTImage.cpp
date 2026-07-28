/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef ENABLE_JS_AOT

#  include "jit/AOTImage.h"

#  include <cstring>

#  include "jit/JitOptions.h"
#  include "jit/JitSpewer.h"

// Symbols exported by the image shim, which embeds either a recorded image or
// the empty placeholder the build generates into the objdir.
extern "C" {
extern const uint8_t aot_image_start[];
extern const uint8_t aot_image_end[];
}

namespace js::jit {

const AOTImage* AOTImage::embedded() {
  static const AOTImage* cached = nullptr;
  static bool initialized = false;
  if (initialized) {
    return cached;
  }
  initialized = true;

  size_t size = size_t(aot_image_end - aot_image_start);
  auto img = fromBytes({aot_image_start, size});
  if (img.isNothing()) {
    JitSpew(JitSpew_BaselineAOT,
            "AOT image absent or invalid (size=%zu); using runtime codegen",
            size);
    return nullptr;
  }

  static AOTImage sImage = img.value();
  cached = &sImage;
  return cached;
}

mozilla::Maybe<AOTImage> AOTImage::fromBytes(
    mozilla::Span<const uint8_t> bytes) {
  if (bytes.size() < sizeof(image::Header)) {
    return mozilla::Nothing();
  }
  AOTImage img(bytes);
  const image::Header* h = img.header();
  if (h->magic != image::Magic || h->version != image::Version) {
    return mozilla::Nothing();
  }
  if (h->imageSize > bytes.size()) {
    return mozilla::Nothing();
  }
  return mozilla::Some(img);
}

mozilla::Maybe<AOTBlobReader> AOTImage::findUnique(AOTBlobKind kind) const {
  const auto* dir = directory();
  const uint8_t* textBase = base_ + header()->textOffset;
  for (uint32_t i = 0; i < blobCount(); i++) {
    if (AOTBlobKind(dir[i].kind) == kind) {
      return mozilla::Some(AOTBlobReader(&dir[i], base_, textBase));
    }
  }
  return mozilla::Nothing();
}

mozilla::Maybe<AOTBlobReader> AOTImage::findByIdentity(
    AOTBlobKind kind, uint32_t probeHash, const uint8_t* identityHash) const {
  const auto* dir = directory();
  const uint8_t* textBase = base_ + header()->textOffset;
  for (uint32_t i = 0; i < blobCount(); i++) {
    const auto& e = dir[i];
    if (AOTBlobKind(e.kind) != kind) continue;
    if (probeHash && e.probeHash != probeHash) continue;
    if (identityHash &&
        memcmp(e.identityHash, identityHash, sizeof(e.identityHash)) != 0) {
      continue;
    }
    return mozilla::Some(AOTBlobReader(&e, base_, textBase));
  }
  return mozilla::Nothing();
}

static uint32_t AlignUp(uint32_t v, uint32_t a) {
  return (v + (a - 1)) & ~(a - 1);
}

// Computes byte offsets for every section of a finalized image. Keeping layout
// separate allows stream and buffer output to share the calculation.
namespace {
struct ImageLayout {
  uint32_t fingerprintOffset;
  uint32_t directoryOffset;
  uint32_t dataStart;
  Vector<image::DirectoryEntry, 0, SystemAllocPolicy> entries;
  uint32_t dataEnd;
  uint32_t textOffset;
  uint32_t textSize;
  uint32_t imageSize;
};
}  // namespace

static bool ComputeLayout(
    const Vector<AOTBlobWriter, 0, SystemAllocPolicy>& blobs,
    ImageLayout& out) {
  out.fingerprintOffset = sizeof(image::Header);
  uint32_t afterFingerprint = out.fingerprintOffset + image::FingerprintSize;
  out.directoryOffset = AlignUp(afterFingerprint, image::Alignment);

  uint32_t cursor =
      out.directoryOffset +
      uint32_t(blobs.length()) * uint32_t(sizeof(image::DirectoryEntry));
  cursor = AlignUp(cursor, image::Alignment);
  out.dataStart = cursor;

  uint32_t textCursor = 0;
  if (!out.entries.reserve(blobs.length())) {
    return false;
  }
  for (const auto& b : blobs) {
    image::DirectoryEntry e = {};
    e.kind = uint32_t(b.kind());
    e.probeHash = b.probeHash();
    memcpy(e.identityHash, b.identityHash(), sizeof(e.identityHash));
    e.dataOffset = cursor;
    e.fieldsSize = uint32_t(b.fields().size());
    e.arraysSize = uint32_t(b.arrays().size());
    e.textSize = uint32_t(b.code().size());
    e.textOffset = textCursor;
    cursor = AlignUp(cursor + e.fieldsSize + e.arraysSize, image::Alignment);
    textCursor += e.textSize;
    out.entries.infallibleAppend(e);
  }
  out.dataEnd = cursor;
  out.textOffset = AlignUp(cursor, image::TextAlignment);
  out.textSize = textCursor;
  out.imageSize = out.textOffset + out.textSize;
  return true;
}

static void WriteHeader(uint8_t* dst, const ImageLayout& layout,
                        uint32_t blobCount) {
  image::Header h = {};
  h.magic = image::Magic;
  h.version = image::Version;
  h.reserved = 0;
  h.blobCount = blobCount;
  h.fingerprintOffset = layout.fingerprintOffset;
  h.fingerprintSize = image::FingerprintSize;
  h.directoryOffset = layout.directoryOffset;
  h.textOffset = layout.textOffset;
  h.textSize = layout.textSize;
  h.imageSize = layout.imageSize;
  memcpy(dst, &h, sizeof(h));
}

bool AOTImageBuilder::finalize(Vector<uint8_t, 0, SystemAllocPolicy>& out,
                               const uint8_t* fingerprint) {
  ImageLayout layout;
  if (!ComputeLayout(blobs_, layout)) return false;
  if (!out.resizeUninitialized(layout.imageSize)) return false;
  memset(out.begin(), 0, layout.imageSize);

  uint8_t* base = out.begin();
  WriteHeader(base, layout, uint32_t(blobs_.length()));
  memcpy(base + layout.fingerprintOffset, fingerprint, image::FingerprintSize);
  memcpy(base + layout.directoryOffset, layout.entries.begin(),
         layout.entries.length() * sizeof(image::DirectoryEntry));
  for (size_t i = 0; i < blobs_.length(); i++) {
    const auto& b = blobs_[i];
    const auto& e = layout.entries[i];
    if (e.fieldsSize) {
      memcpy(base + e.dataOffset, b.fields().data(), e.fieldsSize);
    }
    if (e.arraysSize) {
      memcpy(base + e.dataOffset + e.fieldsSize, b.arrays().data(),
             e.arraysSize);
    }
    if (e.textSize) {
      memcpy(base + layout.textOffset + e.textOffset, b.code().data(),
             e.textSize);
    }
  }
  return true;
}

bool AOTImageBuilder::finalize(std::ostream& out, const uint8_t* fingerprint) {
  Vector<uint8_t, 0, SystemAllocPolicy> buffer;
  if (!finalize(buffer, fingerprint)) return false;
  out.write(reinterpret_cast<const char*>(buffer.begin()),
            std::streamsize(buffer.length()));
  return bool(out);
}

AOTConfigurationMetadata CurrentAOTConfiguration() {
  return {
      .disableInlining = JitOptions.disableInlining,
      .spectreObjectMitigations = JitOptions.spectreObjectMitigations,
      .spectreStringMitigations = JitOptions.spectreStringMitigations,
      .baselineBatching = JitOptions.baselineBatching,
      .baselineJitWarmUpThreshold = JitOptions.baselineJitWarmUpThreshold,
      .baselineQueueCapacity = JitOptions.baselineQueueCapacity,
      .trialInliningWarmUpThreshold = JitOptions.trialInliningWarmUpThreshold,
  };
}

}  // namespace js::jit

#endif  // ENABLE_JS_AOT
