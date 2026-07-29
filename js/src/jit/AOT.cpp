/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/AOT.h"

#include "mozilla/Assertions.h"
#include "mozilla/Maybe.h"

#include "jit/JitContext.h"
#include "jit/JitRuntime.h"
#include "jit/JitSpewer.h"
#include "vm/JSContext.h"
#include "vm/Runtime.h"

#if defined(JS_CODEGEN_X64)
#  include "jit/x64/Assembler-x64.h"
#endif

#include "jit/AOTABIFns-inl.h"

namespace js::jit {

const char* AOTSlotName(AOTSlot slot) {
  switch (slot) {
#define AOT_SLOT(name, ...) \
  case AOTSlot::name:       \
    return #name;
#define AOT_ATOM_SLOT AOT_SLOT
#define AOT_LINK_SLOT AOT_SLOT
#include "jit/AOTSlots.tbl"
#undef AOT_LINK_SLOT
#undef AOT_ATOM_SLOT
#undef AOT_SLOT
    case AOTSlot::InterruptBitsValue:
      return "InterruptBitsValue";
    case AOTSlot::JitStackLimitValue:
      return "JitStackLimitValue";
    case AOTSlot::PreBarrierZoneCount:
      return "PreBarrierZoneCount";
    default:
      break;
  }
  uint32_t s = uint32_t(slot);
  if (s >= uint32_t(AOTSlot::VMWrapper_Begin) &&
      s < uint32_t(AOTSlot::VMWrapper_End)) {
    return "VMWrapper";
  }
  if (s >= uint32_t(AOTSlot::ABIFn_Begin) && s < uint32_t(AOTSlot::ABIFn_End)) {
    return "ABIFn";
  }
  return "Unknown";
}

bool IsAOTLinkSlot(AOTSlot slot) {
  if (IsNamedAOTLinkSlot(slot)) {
    return true;
  }
#ifdef JS_CODEGEN_X64
  uint32_t s = uint32_t(slot);
  if (s >= uint32_t(AOTSlot::ABIFn_Begin) && s < uint32_t(AOTSlot::ABIFn_End)) {
    uint32_t idx = s - uint32_t(AOTSlot::ABIFn_Begin);
    return idx < kAOTABIFnCount && kAOTABIFnLinkable[idx];
  }
#endif
  return false;
}

mozilla::Maybe<AOTSlot> AOTIndirectionTable::findSlot(uintptr_t value) const {
  if (value == 0) {
    return mozilla::Nothing();
  }
  // Mirror slots hold arbitrary values that could collide with a pointer
  // being looked up, so they are excluded from the reverse search.
  for (uint32_t i = 0; i < uint32_t(AOTSlot::Mirror_Begin); i++) {
    if (slots_[i] == value) {
      return mozilla::Some(AOTSlot(i));
    }
  }
  return mozilla::Nothing();
}

mozilla::Maybe<AOTSlot> AOTIndirectionTable::findAtomSlot(
    uintptr_t value) const {
  if (value == 0) {
    return mozilla::Nothing();
  }
#define AOT_SLOT(name, ...)
#define AOT_LINK_SLOT(name, ...)
#define AOT_ATOM_SLOT(name, ...)                  \
  if (slots_[uint32_t(AOTSlot::name)] == value) { \
    return mozilla::Some(AOTSlot::name);          \
  }
#include "jit/AOTSlots.tbl"
#undef AOT_ATOM_SLOT
#undef AOT_LINK_SLOT
#undef AOT_SLOT
  return mozilla::Nothing();
}

void AOTIndirectionTable::dump() const {
  for (uint32_t i = 0; i < uint32_t(AOTSlot::Count); i++) {
    JitSpew(JitSpew_BaselineAOT, "  slot[%u] %-30s = %p", i,
            AOTSlotName(AOTSlot(i)), reinterpret_cast<void*>(get(AOTSlot(i))));
  }
}

AOTSlot AOTIndirectionTable::findSlotOrCrash(uintptr_t value) const {
  auto slot = findSlot(value);
  if (!slot) {
    JitSpew(JitSpew_BaselineAOT, "No AOT slot for %p",
            reinterpret_cast<void*>(value));
    dump();
    MOZ_CRASH("No AOT slot for pointer");
  }
  return *slot;
}

bool EnsureAOTPreambleTrampolineFor(JSContext* cx, JitCode* code,
                                    Register passReg) {
  JitRuntime* jrt = cx->runtime()->jitRuntime();
  if (jrt->lookupAOTPreambleTrampoline(code->raw())) {
    return true;
  }

  mozilla::Maybe<JitContext> jctx;
  if (!MaybeGetJitContext()) {
    jctx.emplace(cx);
  }
  JitCode* trampoline =
      jrt->generateAOTPreambleTrampoline(cx, code->raw(), passReg);
  if (!trampoline) {
    return false;
  }
  if (!jrt->aotPreambleTrampolines_.append(
          JitRuntime::AOTPreambleTrampolineEntry{code->raw(), trampoline})) {
    ReportOutOfMemory(cx);
    return false;
  }
  return true;
}

}  // namespace js::jit
