/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOTImage_h
#define jit_AOTImage_h

#ifdef ENABLE_JS_AOT

#  include "mozilla/Assertions.h"
#  include "mozilla/DebugOnly.h"
#  include "mozilla/Maybe.h"
#  include "mozilla/Span.h"

#  include <cstdint>
#  include <cstring>
#  include <ostream>
#  include <type_traits>

#  include "js/AllocPolicy.h"
#  include "js/Vector.h"

namespace js::jit {

// [SMDOC] AOT Image
// =================
//
// An AOT image is a flat binary produced at build time and mapped when the
// process starts. It contains a header, a fingerprint, an artifact directory,
// serialized metadata, padding, and a page aligned code segment.
//
//   +---------------------------------------------------+
//   | AOTImageHeader     (fixed layout, HeaderSize)    |
//   +---------------------------------------------------+
//   | Fingerprint bytes  (header.fingerprintSize)       |
//   +---------------------------------------------------+
//   | AOTBlobDirectoryEntry[header.blobCount]           |
//   +---------------------------------------------------+
//   | Per-blob { fields POD, arrays } sections          |
//   +---------------------------------------------------+
//   | [padding to page alignment]                       |
//   +---------------------------------------------------+
//   | Text segment  (concatenated code, page-aligned)   |
//   +---------------------------------------------------+
//
// Each artifact contains the metadata required to reconstruct its runtime
// object. A shared schema defines the serialized layout used by readers and
// writers.
//
// The loader validates the header and fingerprint before exposing artifacts.
// The builder emits a finalized image from recorded artifacts.

class AOTImage;
class AOTBlobReader;
class AOTBlobWriter;
class AOTImageBuilder;

// Artifact kinds are defined by the image schema. Adding a kind requires
// updating the enumeration and image format version.
enum class AOTBlobKind : uint32_t {
  BaselineInterpreter = 0,
  BaselineFunction = 1,
  InlineCacheStub = 2,
  Configuration = 3,
};

namespace image {

// "AOTI" in little-endian.
inline constexpr uint32_t Magic = 0x49544F41;

// Increment the image format version whenever the layout, schema, or
// fingerprint inputs change.
inline constexpr uint16_t Version = 3;

// The fingerprint covers all engine inputs that affect generated code and is
// checked when an image is loaded.
inline constexpr uint32_t FingerprintSize = 20;

// Align directory metadata to a cache line boundary.
inline constexpr uint32_t Alignment = 16;

// Align generated code to a page boundary so its protection can change without
// affecting image metadata.
inline constexpr uint32_t TextAlignment = 4096;

struct Header {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  uint32_t blobCount;
  uint32_t fingerprintOffset;
  uint32_t fingerprintSize;
  uint32_t directoryOffset;
  uint32_t textOffset;
  uint32_t textSize;
  uint32_t imageSize;
};

static_assert(sizeof(Header) == 36,
              "image::Header wire size; edit image::Version on change");

struct DirectoryEntry {
  uint32_t kind;
  // Stores a short prefix of the identity hash for fast rejection during
  // baseline artifact lookup. Other artifact kinds store zero.
  uint32_t probeHash;
  uint8_t identityHash[20];
  uint32_t textOffset;
  uint32_t textSize;
  uint32_t dataOffset;
  uint32_t fieldsSize;
  uint32_t arraysSize;
};

static_assert(sizeof(DirectoryEntry) == 48,
              "image::DirectoryEntry wire size; edit image::Version on change");

}  // namespace image

// Reads the serialized fields and arrays for one artifact in order.
class AOTBlobReader {
 public:
  AOTBlobReader(const image::DirectoryEntry* entry, const uint8_t* imageBase,
                const uint8_t* textBase)
      : entry_(entry),
        code_(textBase + entry->textOffset, entry->textSize),
        fields_(imageBase + entry->dataOffset),
        arraysCursor_(imageBase + entry->dataOffset + entry->fieldsSize),
        arraysEnd_(arraysCursor_ + entry->arraysSize) {}

  AOTBlobKind kind() const { return AOTBlobKind(entry_->kind); }
  const image::DirectoryEntry* entry() const { return entry_; }
  uint32_t fieldsSize() const { return entry_->fieldsSize; }
  mozilla::Span<const uint8_t> code() const { return code_; }
  mozilla::Span<const uint8_t> identityHash() const {
    return {entry_->identityHash, sizeof(entry_->identityHash)};
  }

  template <typename T>
  T readFields() {
    static_assert(std::is_trivially_copyable_v<T>,
                  "AOT fields POD must be trivially copyable");
    MOZ_ASSERT(fieldsSize() == sizeof(T));
    T out;
    memcpy(&out, fields_, sizeof(T));
    return out;
  }

  template <typename T>
  mozilla::Span<const T> readArray(uint32_t count) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "AOT array elements must be trivially copyable");
    if (count == 0) {
      return {};
    }
    MOZ_ASSERT(arraysCursor_ + count * sizeof(T) <= arraysEnd_);
    auto span = mozilla::Span(reinterpret_cast<const T*>(arraysCursor_), count);
    arraysCursor_ += count * sizeof(T);
    return span;
  }

 private:
  const image::DirectoryEntry* entry_;
  mozilla::Span<const uint8_t> code_;
  const uint8_t* fields_;
  const uint8_t* arraysCursor_;
  mozilla::DebugOnly<const uint8_t*> arraysEnd_;
};

// Collects one artifact's code, fixed fields, and array data before adding it
// to the image.
class AOTBlobWriter {
 public:
  AOTBlobWriter(AOTBlobKind kind, uint32_t probeHash,
                const uint8_t identityHash[20])
      : kind_(kind), probeHash_(probeHash) {
    if (identityHash) {
      memcpy(identityHash_, identityHash, sizeof(identityHash_));
    } else {
      memset(identityHash_, 0, sizeof(identityHash_));
    }
  }

  AOTBlobKind kind() const { return kind_; }
  uint32_t probeHash() const { return probeHash_; }
  const uint8_t* identityHash() const { return identityHash_; }

  mozilla::Span<const uint8_t> code() const {
    return {code_.begin(), code_.length()};
  }
  mozilla::Span<const uint8_t> fields() const {
    return {fields_.begin(), fields_.length()};
  }
  mozilla::Span<const uint8_t> arrays() const {
    return {arrays_.begin(), arrays_.length()};
  }

  [[nodiscard]] bool writeCode(const uint8_t* data, size_t len) {
    return code_.append(data, len);
  }

  template <typename T>
  [[nodiscard]] bool writeFields(const T& f) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "AOT fields POD must be trivially copyable");
    return fields_.append(reinterpret_cast<const uint8_t*>(&f), sizeof(T));
  }

  template <typename T>
  [[nodiscard]] bool writeArray(const T* data, size_t count) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "AOT array elements must be trivially copyable");
    if (count == 0) {
      return true;
    }
    return arrays_.append(reinterpret_cast<const uint8_t*>(data),
                          count * sizeof(T));
  }

 private:
  AOTBlobKind kind_;
  uint32_t probeHash_;
  uint8_t identityHash_[20];
  Vector<uint8_t, 0, SystemAllocPolicy> code_;
  Vector<uint8_t, 0, SystemAllocPolicy> fields_;
  Vector<uint8_t, 0, SystemAllocPolicy> arrays_;
};

// Provides a read only view of an image embedded in the binary or supplied by a
// test.
class AOTImage {
 public:
  // No embedded image is available when the binary contains no linked image
  // symbols.
  static const AOTImage* embedded();

  static mozilla::Maybe<AOTImage> fromBytes(mozilla::Span<const uint8_t> bytes);

  const image::Header* header() const {
    return reinterpret_cast<const image::Header*>(base_);
  }
  mozilla::Span<const uint8_t> fingerprint() const {
    return {base_ + header()->fingerprintOffset, header()->fingerprintSize};
  }
  mozilla::Span<const uint8_t> text() const {
    return {base_ + header()->textOffset, header()->textSize};
  }
  uint32_t blobCount() const { return header()->blobCount; }

  AOTBlobReader blobAt(uint32_t index) const {
    MOZ_ASSERT(index < blobCount());
    return AOTBlobReader(directory() + index, base_,
                         base_ + header()->textOffset);
  }

  const image::DirectoryEntry* directory() const {
    return reinterpret_cast<const image::DirectoryEntry*>(
        base_ + header()->directoryOffset);
  }

  // Finds the only artifact of a requested kind.
  mozilla::Maybe<AOTBlobReader> findUnique(AOTBlobKind kind) const;

  // Finds an artifact by kind and identity hash. A zero probe value disables
  // the fast rejection step.
  mozilla::Maybe<AOTBlobReader> findByIdentity(
      AOTBlobKind kind, uint32_t probeHash, const uint8_t* identityHash) const;

 private:
  explicit AOTImage(mozilla::Span<const uint8_t> bytes)
      : base_(bytes.data()), size_(bytes.size()) {}

  const uint8_t* base_;
  size_t size_;
};

// Defines the intermediate format for one recorded artifact. Each file contains
// a fixed header followed by fields, arrays, code, and link sites in image
// directory order. Both the recorder and packer use this format. Link sites
// stop at the packer, which turns them into relocations the static linker has
// already applied by the time the runtime sees the image bytes.
struct AOTBlobFileHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  uint32_t kind;
  uint32_t probeHash;
  uint8_t identityHash[20];
  uint32_t fieldsSize;
  uint32_t arraysSize;
  uint32_t codeSize;
  uint32_t linkSitesSize;
  // Identifies the slot numbering the link sites were recorded against.
  uint32_t slotTableHash;
};

static_assert(sizeof(AOTBlobFileHeader) == 56,
              "AOTBlobFileHeader wire size; edit BlobFileVersion on change");

// "AOTB" in little-endian.
inline constexpr uint32_t BlobFileMagic = 0x42544F41;
inline constexpr uint16_t BlobFileVersion = 2;

// Builds an image in memory from recorded artifacts using a supplied
// fingerprint.
class AOTImageBuilder {
 public:
  [[nodiscard]] bool addBlob(AOTBlobWriter&& blob) {
    return blobs_.append(std::move(blob));
  }

  uint32_t blobCount() const { return blobs_.length(); }

  // Writes a finalized image. The fingerprint must have the expected length.
  [[nodiscard]] bool finalize(std::ostream& out, const uint8_t* fingerprint);

  // Collects a finalized image in memory for tests.
  [[nodiscard]] bool finalize(Vector<uint8_t, 0, SystemAllocPolicy>& out,
                              const uint8_t* fingerprint);

 private:
  Vector<AOTBlobWriter, 0, SystemAllocPolicy> blobs_;
};

// Holds the runtime representation of a serialized inline cache stub. Encoding
// copies the relevant stub metadata. Decoding reconstructs the metadata and
// lookup key. This keeps inline cache types out of the schema generator.
struct AOTICStubMetadata {
  uint8_t cacheKind = 0;
  uint8_t makesGCCalls = 0;
  uint8_t stubDataOffset = 0;
  uint8_t localTracingSlots = 0;
  Vector<uint8_t, 0, SystemAllocPolicy> cacheIRCode;
  Vector<uint8_t, 0, SystemAllocPolicy> fieldTypes;
};

struct AOTConfigurationMetadata {
  uint8_t disableInlining = 0;
  uint8_t spectreObjectMitigations = 0;
  uint8_t spectreStringMitigations = 0;
  uint8_t baselineBatching = 0;
  uint32_t baselineJitWarmUpThreshold = 0;
  uint32_t baselineQueueCapacity = 0;
  uint32_t trialInliningWarmUpThreshold = 0;

  bool operator==(const AOTConfigurationMetadata& other) const = default;
};

AOTConfigurationMetadata CurrentAOTConfiguration();

}  // namespace js::jit

#endif  // ENABLE_JS_AOT

#endif  // jit_AOTImage_h
