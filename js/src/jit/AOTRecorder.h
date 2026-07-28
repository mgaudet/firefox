/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOTRecorder_h
#define jit_AOTRecorder_h

#ifdef ENABLE_JS_AOT

#  include "mozilla/HashTable.h"
#  include "mozilla/Span.h"

#  include <cstdint>
#  include <string>

#  include "jstypes.h"

#  include "jit/AOT.h"
#  include "jit/AOTImage.h"
#  include "js/AllocPolicy.h"

struct JS_PUBLIC_API JSContext;

namespace js::jit {

class JitCode;
class BaselineInterpreter;
struct BaselineInterpreterMetadata;
struct BaselineScriptMetadata;
struct AOTICStubMetadata;

// [SMDOC] AOT Artifact Recorder
// =============================
//
// Records captured AOT artifacts as individual files. Each file contains
// generated code and the metadata required to rebuild the corresponding
// runtime object. Identity based names and exclusive creation deduplicate
// artifacts across concurrent processes.
class AOTArtifactRecorder {
 public:
  AOTArtifactRecorder() = default;
  AOTArtifactRecorder(const AOTArtifactRecorder&) = delete;
  AOTArtifactRecorder& operator=(const AOTArtifactRecorder&) = delete;

  // Creates the recording directory if it does not already exist.
  [[nodiscard]] bool init(JSContext* cx, const char* dir);

  const std::string& directory() const { return directory_; }

  // Each record entry point takes the link sites the capturing assembler
  // collected. Their offsets are relative to the artifact's own code.

  // The baseline interpreter has a fixed artifact name because only one is
  // recorded.
  [[nodiscard]] bool recordInterpreter(JSContext* cx, JitCode* code,
                                       const BaselineInterpreterMetadata& md,
                                       mozilla::Span<const AOTLinkSite> sites);

  // Baseline function artifacts are named from a hash of the script state that
  // affects compilation.
  [[nodiscard]] bool recordBaselineFunction(
      JSContext* cx, JitCode* code, const uint8_t identityHash[20],
      uint32_t probeHash, const BaselineScriptMetadata& md,
      mozilla::Span<const AOTLinkSite> sites);

  // Inline cache identities cover the cache kind, encoded operations, and field
  // types. Artifact file names include a prefix of that hash.
  [[nodiscard]] bool recordICStub(JSContext* cx, JitCode* code,
                                  const AOTICStubMetadata& md,
                                  mozilla::Span<const AOTLinkSite> sites);

  // Records each self hosted function by delazifying it and triggering baseline
  // compilation. Returns the number of artifacts recorded. An empty result is
  // valid.
  [[nodiscard]] bool recordSelfHostedBaselineCorpus(JSContext* cx,
                                                    uint32_t* compiledOut,
                                                    uint32_t* skippedOut);

 private:
  using SeenSet = mozilla::HashSet<uint64_t, mozilla::DefaultHasher<uint64_t>,
                                   SystemAllocPolicy>;

  [[nodiscard]] bool writeBlobFile(JSContext* cx, const std::string& path,
                                   const AOTBlobWriter& blob,
                                   mozilla::Span<const AOTLinkSite> sites);

  // Duplicate artifacts within a process are filtered in memory. Exclusive file
  // creation handles duplicates from other processes.
  bool wasSeen(const uint8_t identityHash[20]);

  std::string directory_;
  SeenSet seen_;
};

}  // namespace js::jit

#endif  // ENABLE_JS_AOT

#endif  // jit_AOTRecorder_h
