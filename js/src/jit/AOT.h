/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOT_h
#define jit_AOT_h

#include "mozilla/Assertions.h"
#include "mozilla/Maybe.h"

#include <cstdint>

#include "jstypes.h"

#include "jit/Registers.h"

struct JS_PUBLIC_API JSContext;

namespace js::jit {

class JitCode;

extern const double MathRandomScaleInv;

// [SMDOC] AOT JIT Code
//
// SpiderMonkey can emit relocatable code for the baseline interpreter, inline
// cache stubs, and self hosted functions. Runtime addresses are loaded through
// an indirection table so generated code does not embed process specific
// pointers. Baseline and stub frames each store the table address used by
// generated code.

// Reserves fixed index ranges for groups of runtime supplied entries. The
// limits are intentionally conservative and checked when the table is
// populated.
static constexpr uint32_t AOTMaxVMWrappers = 512;
static constexpr uint32_t AOTMaxABIFunctions = 256;

enum class AOTSlot : uint32_t {
#define AOT_SLOT(name, ...) name,
#define AOT_ATOM_SLOT AOT_SLOT
#define AOT_LINK_SLOT AOT_SLOT
#include "jit/AOTSlots.tbl"
#undef AOT_LINK_SLOT
#undef AOT_ATOM_SLOT
#undef AOT_SLOT
  NamedSlot_End,
  VMWrapper_Begin = NamedSlot_End,
  VMWrapper_End = VMWrapper_Begin + AOTMaxVMWrappers,
  ABIFn_Begin = VMWrapper_End,
  ABIFn_End = ABIFn_Begin + AOTMaxABIFunctions,
  // Mirrored values rather than addresses. The runtime rewrites these
  // whenever the source of truth changes, saving generated code a
  // dereference on hot checks.
  Mirror_Begin = ABIFn_End,
  InterruptBitsValue = Mirror_Begin,
  JitStackLimitValue,
  PreBarrierZoneCount,
  Mirror_End,
  Count = Mirror_End
};

inline AOTSlot AOTSlotForVMWrapper(uint32_t id) {
  MOZ_ASSERT(id < AOTMaxVMWrappers);
  return AOTSlot(uint32_t(AOTSlot::VMWrapper_Begin) + id);
}

inline AOTSlot AOTSlotForABIFn(uint32_t idx) {
  MOZ_ASSERT(idx < AOTMaxABIFunctions);
  return AOTSlot(uint32_t(AOTSlot::ABIFn_Begin) + idx);
}

// True when the slot's value is fixed at link time, so generated code can
// reference it with a relocation the static linker resolves rather than an
// indirection table load. AOTLinkSyms.h must carry a specialization for every
// such slot.
constexpr bool IsAOTLinkSlot(AOTSlot slot) {
  switch (slot) {
#define AOT_SLOT(name, ...)
#define AOT_ATOM_SLOT(name, ...)
#define AOT_LINK_SLOT(name, ...) case AOTSlot::name:
#include "jit/AOTSlots.tbl"
#undef AOT_LINK_SLOT
#undef AOT_ATOM_SLOT
#undef AOT_SLOT
    return true;
    default:
      return false;
  }
}

// Identifies the slot numbering this build compiles against. Link sites name
// slots by index, so an image recorded against a different numbering would
// bind to the wrong symbol. Hashing the effective slot list rather than the
// table source also covers conditionally compiled entries, which shift every
// following index.
constexpr uint32_t AOTSlotTableHash() {
  const char* const names[] = {
#define AOT_SLOT(name, ...) #name,
#define AOT_ATOM_SLOT(name, ...) "@" #name,
#define AOT_LINK_SLOT(name, ...) "&" #name,
#include "jit/AOTSlots.tbl"
#undef AOT_LINK_SLOT
#undef AOT_ATOM_SLOT
#undef AOT_SLOT
  };
  uint32_t h = 2166136261u;
  auto mix = [&h](uint8_t b) { h = (h ^ b) * 16777619u; };
  for (const char* n : names) {
    for (; *n; n++) {
      mix(uint8_t(*n));
    }
    mix('|');
  }
  const uint32_t limits[] = {AOTMaxVMWrappers, AOTMaxABIFunctions,
                             uint32_t(AOTSlot::Count)};
  for (uint32_t v : limits) {
    for (int i = 0; i < 4; i++) {
      mix(uint8_t(v >> (8 * i)));
    }
  }
  return h;
}

const char* AOTSlotName(AOTSlot slot);

// A four byte rip relative displacement inside recorded code that the next
// build's static linker fills in from the slot's symbol. The displacement is
// left zero at capture time; the recorded bytes are never executed.
struct AOTLinkSite {
  uint32_t codeOffset;
  uint32_t slot;
};

static_assert(sizeof(AOTLinkSite) == 8, "AOTLinkSite is written to .aotb");

class AOTIndirectionTable {
 public:
  AOTIndirectionTable() = default;

  void set(AOTSlot slot, uintptr_t value) {
    MOZ_ASSERT(uint32_t(slot) < uint32_t(AOTSlot::Count));
    slots_[uint32_t(slot)] = value;
  }

  uintptr_t get(AOTSlot slot) const {
    MOZ_ASSERT(uint32_t(slot) < uint32_t(AOTSlot::Count));
    return slots_[uint32_t(slot)];
  }

  static constexpr bool isMirrorSlot(AOTSlot slot) {
    return uint32_t(slot) >= uint32_t(AOTSlot::Mirror_Begin) &&
           uint32_t(slot) < uint32_t(AOTSlot::Mirror_End);
  }

  // Mirror slots may be rewritten from another thread while jit code on the
  // owning thread polls them.
  void setMirrored(AOTSlot slot, uintptr_t value) {
    MOZ_ASSERT(isMirrorSlot(slot));
    __atomic_store_n(&slots_[uint32_t(slot)], value, __ATOMIC_SEQ_CST);
  }

  static constexpr uint32_t offsetOfSlot(AOTSlot slot) {
    return uint32_t(slot) * sizeof(uintptr_t);
  }

  mozilla::Maybe<AOTSlot> findSlot(uintptr_t value) const;
  mozilla::Maybe<AOTSlot> findAtomSlot(uintptr_t value) const;
  AOTSlot findSlotOrCrash(uintptr_t value) const;
  void dump() const;

  uintptr_t* baseAddress() { return slots_; }
  const uintptr_t* baseAddress() const { return slots_; }

 private:
  uintptr_t slots_[uint32_t(AOTSlot::Count)] = {};
};

// Creates a preamble for static code when needed. The preamble initializes the
// runtime indirection table and enters the target using the register expected
// by that entry path. Repeated requests return the existing preamble.
[[nodiscard]] bool EnsureAOTPreambleTrampolineFor(JSContext* cx, JitCode* code,
                                                  Register passReg);

}  // namespace js::jit

#endif  // jit_AOT_h
