/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef ENABLE_JS_AOT

#  include "jit/AOTRecorder.h"

#  include "mozilla/ScopeExit.h"
#  include "mozilla/SHA1.h"

#  include <cerrno>
#  include <cstdio>
#  include <cstring>
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>

#  include "frontend/CompilationStencil.h"
#  include "gc/Zone.h"
#  include "jit/AOTImageGenerated.h"
#  include "jit/BaselineJIT.h"
#  include "jit/CacheIR.h"
#  include "jit/JitCode.h"
#  include "jit/JitScript.h"
#  include "jit/JitSpewer.h"
#  include "jit/JitZone.h"
#  include "vm/JSAtomUtils.h"
#  include "vm/JSContext.h"
#  include "vm/JSFunction.h"
#  include "vm/JSScript.h"
#  include "vm/Runtime.h"

#  include "jit/AOTABIFns-inl.h"
#  include "jit/JitScript-inl.h"
#  include "vm/JSObject-inl.h"

namespace js::jit {

// Filename helpers

static void HexEncode(const uint8_t* bytes, size_t len, char* out) {
  static const char Hex[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[2 * i] = Hex[bytes[i] >> 4];
    out[2 * i + 1] = Hex[bytes[i] & 0x0f];
  }
  out[2 * len] = '\0';
}

static uint64_t Prefix64(const uint8_t* bytes) {
  uint64_t v;
  memcpy(&v, bytes, sizeof(v));
  return v;
}

// AOTArtifactRecorder

bool AOTArtifactRecorder::init(JSContext* cx, const char* dir) {
  directory_.assign(dir);
  if (mkdir(directory_.c_str(), 0755) != 0 && errno != EEXIST) {
    JitSpew(JitSpew_BaselineAOT, "AOT record dir mkdir failed: %s: %s",
            directory_.c_str(), strerror(errno));
    return false;
  }
  AOTBlobWriter blob(AOTBlobKind::Configuration, /* probeHash = */ 0,
                     /* identityHash = */ nullptr);
  if (!EncodeBlob_Configuration(blob, CurrentAOTConfiguration())) {
    return false;
  }
  return writeBlobFile(cx, directory_ + "/configuration.aotb", blob, {});
}

// Recorded bytes are executed by a later build at an address this one cannot
// know, so anything the linker would have had to patch is a runtime address
// that escaped the indirection table. On x86 such a target is a displacement
// the recorded bytes carry silently; on a fixed-width ISA it is worse, because
// arm64's Assembler::finish appends an extended jump table of absolute
// pointers to the code buffer itself. Neither survives being packed into an
// image, so refuse to record instead of shipping code that jumps into the
// recording process's address space.
static void AssertNoAbsoluteRelocations(JitCode* code) {
  MOZ_RELEASE_ASSERT(code->jumpRelocTableBytes() == 0,
                     "AOT-recorded code has jump relocations");
  MOZ_RELEASE_ASSERT(code->dataRelocTableBytes() == 0,
                     "AOT-recorded code has data relocations");
}

#  if defined(JS_CODEGEN_ARM64) && defined(XP_LINUX)

// A pointer materialized as a bare immediate leaves no relocation and never
// passes through movePtr, so neither the check above nor the interception
// layer's unknown-pointer crash can see it. The recorded bytes simply carry
// the recording process's address, and the next process reads a stale one --
// silently, because the code still executes. Catching it needs the recorded
// instructions themselves.
static bool IsMappedInThisProcess(uintptr_t value) {
  static const size_t pageSize = size_t(sysconf(_SC_PAGESIZE));
  unsigned char unused;
  void* page = reinterpret_cast<void*>(value & ~uintptr_t(pageSize - 1));
  return mincore(page, pageSize, &unused) == 0;
}

// Walks movz/movk runs and reports the completed value. Judging a partial run
// would flag every NaN-boxed constant in the corpus, since those finish with
// a `movk ..., lsl #48`.
static void AssertNoBakedAddresses(JitCode* code) {
  // Below this a value is a constant or a bitmask, not an address. Tags sit
  // above the 48-bit userspace ceiling and are excluded by the mapping check.
  static constexpr uintptr_t kMinPlausibleAddress = uintptr_t(1) << 32;

  constexpr uint32_t kMask = 0xFF800000;
  constexpr uint32_t kMovz = 0xD2800000;
  constexpr uint32_t kMovk = 0xF2800000;

  const uint32_t* insns = reinterpret_cast<const uint32_t*>(code->raw());
  size_t count = code->instructionsSize() / sizeof(uint32_t);

  uint64_t pending[32] = {};
  bool live[32] = {};

  auto check = [&](uint32_t reg) {
    uint64_t v = pending[reg];
    if (v >= kMinPlausibleAddress && IsMappedInThisProcess(v)) {
      MOZ_CRASH_UNSAFE_PRINTF(
          "AOT: recorded code bakes the address %p as an immediate; route it "
          "through movePtr so the indirection table or a link slot covers it.",
          reinterpret_cast<void*>(uintptr_t(v)));
    }
  };

  for (size_t i = 0; i < count; i++) {
    uint32_t insn = insns[i];
    uint32_t top = insn & kMask;
    uint32_t hw = (insn >> 21) & 0x3;
    uint64_t imm16 = (insn >> 5) & 0xFFFF;
    uint32_t rd = insn & 0x1F;
    if (top == kMovz) {
      if (live[rd]) {
        check(rd);
      }
      pending[rd] = imm16 << (16 * hw);
      live[rd] = true;
    } else if (top == kMovk && live[rd]) {
      pending[rd] &= ~(uint64_t(0xFFFF) << (16 * hw));
      pending[rd] |= imm16 << (16 * hw);
    } else if (live[rd]) {
      check(rd);
      live[rd] = false;
    }
  }
  for (uint32_t rd = 0; rd < 32; rd++) {
    if (live[rd]) {
      check(rd);
    }
  }
}

#  else
static void AssertNoBakedAddresses(JitCode* code) {}
#  endif

bool AOTArtifactRecorder::wasSeen(const uint8_t identityHash[20]) {
  uint64_t key = Prefix64(identityHash);
  auto p = seen_.lookupForAdd(key);
  if (p) {
    return true;
  }
  (void)seen_.add(p, key);
  return false;
}

bool AOTArtifactRecorder::writeBlobFile(
    JSContext* cx, const std::string& path, const AOTBlobWriter& blob,
    mozilla::Span<const AOTLinkSite> sites) {
  // Artifact names encode identity, so the first successful writer owns the
  // file and concurrent recorders can safely ignore duplicates.
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd < 0) {
    if (errno == EEXIST) {
      return true;
    }
    JitSpew(JitSpew_BaselineAOT, "AOT record open failed: %s: %s", path.c_str(),
            strerror(errno));
    return false;
  }
  auto closeGuard = mozilla::MakeScopeExit([&] { close(fd); });

  AOTBlobFileHeader hdr = {};
  hdr.magic = BlobFileMagic;
  hdr.version = BlobFileVersion;
  hdr.kind = uint32_t(blob.kind());
  hdr.probeHash = blob.probeHash();
  memcpy(hdr.identityHash, blob.identityHash(), sizeof(hdr.identityHash));
  hdr.fieldsSize = uint32_t(blob.fields().size());
  hdr.arraysSize = uint32_t(blob.arrays().size());
  hdr.codeSize = uint32_t(blob.code().size());
  hdr.linkSitesSize = uint32_t(sites.size() * sizeof(AOTLinkSite));
  hdr.slotTableHash = AOTImageLinkHash();

  auto writeBytes = [&](const void* p, size_t n) -> bool {
    const uint8_t* cur = static_cast<const uint8_t*>(p);
    while (n) {
      ssize_t rc = write(fd, cur, n);
      if (rc < 0) {
        if (errno == EINTR) continue;
        JitSpew(JitSpew_BaselineAOT, "AOT record write failed: %s: %s",
                path.c_str(), strerror(errno));
        return false;
      }
      cur += rc;
      n -= size_t(rc);
    }
    return true;
  };

  return writeBytes(&hdr, sizeof(hdr)) &&
         writeBytes(blob.fields().data(), blob.fields().size()) &&
         writeBytes(blob.arrays().data(), blob.arrays().size()) &&
         writeBytes(blob.code().data(), blob.code().size()) &&
         writeBytes(sites.data(), hdr.linkSitesSize);
}

bool AOTArtifactRecorder::recordInterpreter(
    JSContext* cx, JitCode* code, const BaselineInterpreterMetadata& md,
    mozilla::Span<const AOTLinkSite> sites) {
  AOTBlobWriter blob(AOTBlobKind::BaselineInterpreter, /* probeHash = */ 0,
                     /* identityHash = */ nullptr);
  if (!EncodeBlob_BaselineInterpreter(blob, md)) return false;
  AssertNoAbsoluteRelocations(code);
  AssertNoBakedAddresses(code);
  if (!blob.writeCode(code->raw(), code->instructionsSize())) return false;

  std::string path = directory_ + "/interp.aotb";
  return writeBlobFile(cx, path, blob, sites);
}

bool AOTArtifactRecorder::recordBaselineFunction(
    JSContext* cx, JitCode* code, const uint8_t identityHash[20],
    uint32_t probeHash, const BaselineScriptMetadata& md,
    mozilla::Span<const AOTLinkSite> sites) {
  if (wasSeen(identityHash)) return true;

  AOTBlobWriter blob(AOTBlobKind::BaselineFunction, probeHash, identityHash);
  if (!EncodeBlob_BaselineFunction(blob, md)) return false;
  AssertNoAbsoluteRelocations(code);
  AssertNoBakedAddresses(code);
  if (!blob.writeCode(code->raw(), code->instructionsSize())) return false;

  char idHex[41];
  HexEncode(identityHash, 8, idHex);
  std::string path = directory_ + "/blfun-" + idHex + ".aotb";
  return writeBlobFile(cx, path, blob, sites);
}

bool AOTArtifactRecorder::recordSelfHostedBaselineCorpus(JSContext* cx,
                                                         uint32_t* compiledOut,
                                                         uint32_t* skippedOut) {
  *compiledOut = 0;
  *skippedOut = 0;

  if (!cx->runtime()->hasSelfHostStencil()) {
    JitSpew(JitSpew_BaselineAOT,
            "AOT self-hosted corpus: no self-host stencil loaded");
    return true;
  }

  JS::RootedVector<JSAtom*> names(cx);
  {
    auto& map = cx->runtime()->selfHostScriptMap.ref();
    if (!names.reserve(map.count())) {
      return false;
    }
    for (auto iter = map.iter(); !iter.done(); iter.next()) {
      names.infallibleAppend(iter.get().key());
    }
  }

  // Self hosted function instantiation must not invoke the allocation metadata
  // builder.
  AutoSuppressAllocationMetadataBuilder suppressMetadata(cx);

  for (JSAtom* rawAtom : names.get()) {
    Rooted<JSAtom*> atom(cx, rawAtom);
    Rooted<PropertyName*> name(cx, atom->asPropertyName());
    auto indexRange = cx->runtime()->getSelfHostedScriptIndexRange(name);
    if (!indexRange) {
      (*skippedOut)++;
      continue;
    }

    RootedFunction fun(
        cx, cx->runtime()->selfHostStencil().instantiateSelfHostedLazyFunction(
                cx, cx->runtime()->selfHostStencilInput().atomCache,
                indexRange->start, name));
    if (!fun) {
      (*skippedOut)++;
      cx->clearPendingException();
      continue;
    }
    if (!cx->runtime()->delazifySelfHostedFunction(cx, name, fun)) {
      (*skippedOut)++;
      cx->clearPendingException();
      continue;
    }

    // Trigger baseline compilation here so the recorder receives the generated
    // artifact through the normal compilation path.
    Rooted<JSScript*> script(cx, fun->nonLazyScript());
    if (!script || !CanBaselineInterpretScript(script)) {
      (*skippedOut)++;
      continue;
    }
    if (!cx->zone()->ensureJitZoneExists(cx)) {
      return false;
    }
    AutoKeepJitScripts keepJitScript(cx);
    if (!script->ensureHasJitScript(cx, keepJitScript)) {
      (*skippedOut)++;
      cx->clearPendingException();
      continue;
    }

    BaselineOptions options({BaselineOption::ForceMainThreadCompilation});
    MethodStatus status = BaselineCompile(cx, script, options);
    if (status != Method_Compiled) {
      (*skippedOut)++;
      cx->clearPendingException();
      continue;
    }
    (*compiledOut)++;
  }

  JitSpew(JitSpew_BaselineAOT, "AOT self-hosted corpus: recorded=%u skipped=%u",
          *compiledOut, *skippedOut);
  return true;
}

bool AOTArtifactRecorder::recordICStub(JSContext* cx, JitCode* code,
                                       const AOTICStubMetadata& md,
                                       mozilla::Span<const AOTLinkSite> sites) {
  // Hash the inputs that determine generated stub code. Equal identities are
  // safe to deduplicate by file name.
  mozilla::SHA1Sum sha;
  sha.update(&md.cacheKind, sizeof(md.cacheKind));
  if (!md.cacheIRCode.empty()) {
    sha.update(md.cacheIRCode.begin(), md.cacheIRCode.length());
  }
  if (!md.fieldTypes.empty()) {
    sha.update(md.fieldTypes.begin(), md.fieldTypes.length());
  }
  mozilla::SHA1Sum::Hash hash;
  sha.finish(hash);

  static_assert(sizeof(hash) == 20,
                "SHA1 hash width mirrors AOTBlobFileHeader::identityHash");

  if (wasSeen(hash)) return true;

  AOTBlobWriter blob(AOTBlobKind::InlineCacheStub, /* probeHash = */ 0, hash);
  if (!EncodeBlob_InlineCacheStub(blob, md)) return false;
  AssertNoAbsoluteRelocations(code);
  AssertNoBakedAddresses(code);
  if (!blob.writeCode(code->raw(), code->instructionsSize())) return false;

  char idHex[41];
  HexEncode(hash, 8, idHex);
  std::string path = directory_ + "/ic-" + idHex + ".aotb";
  return writeBlobFile(cx, path, blob, sites);
}

}  // namespace js::jit

#endif  // ENABLE_JS_AOT
