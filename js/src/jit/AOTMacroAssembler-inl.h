/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOTMacroAssembler_inl_h
#define jit_AOTMacroAssembler_inl_h

#include "jit/CompileWrappers.h"
#include "jit/MacroAssembler.h"

namespace js::jit {

#ifdef ENABLE_JS_AOT

// Values below this threshold represent sentinels rather than runtime
// addresses, so encode them directly.
static constexpr uintptr_t kAOTBakeableSentinelLimit = 16;

#  define AOT_CRASH_ON_UNKNOWN_PTR(kind, val)                             \
    do {                                                                  \
      if ((val) >= kAOTBakeableSentinelLimit) {                           \
        MOZ_CRASH_UNSAFE_PRINTF("AOT: no indirection slot for " kind      \
                                " %p, add to the AOT indirection table.", \
                                reinterpret_cast<void*>(val));            \
      }                                                                   \
    } while (0)
#endif

inline void MacroAssembler::loadRuntime(Register reg) {
  movePtr(ImmPtr(runtime()), reg);
}

inline void MacroAssembler::loadZoneBase(Register dest) {
#ifdef ENABLE_JS_AOT
  if (isAOT()) {
    loadZoneForAOT(dest);
    return;
  }
#endif
  MacroAssemblerSpecific::movePtr(ImmPtr(realm()->zone()->zone()), dest);
}

inline void MacroAssembler::movePtr(ImmPtr imm, Register dest) {
#ifdef ENABLE_JS_AOT
  if (MOZ_UNLIKELY(isAOT())) {
    uintptr_t val = uintptr_t(imm.value);
    if (auto slot = aotTable().findSlot(val)) {
      if (IsAOTLinkSlot(*slot)) {
        emitAOTLinkAddress(*slot, dest);
      } else {
        emitAOTSlotLoad(*slot, dest);
      }
      return;
    }
    AOT_CRASH_ON_UNKNOWN_PTR("movePtr(ImmPtr)", val);
  }
#endif
  MacroAssemblerSpecific::movePtr(imm, dest);
}

inline void MacroAssembler::movePtr(ImmGCPtr imm, Register dest) {
#ifdef ENABLE_JS_AOT
  if (MOZ_UNLIKELY(isAOT())) {
    if (auto slot = aotTable().findAtomSlot(uintptr_t(imm.value))) {
      emitAOTSlotLoad(*slot, dest);
      return;
    }
    // Other GC pointers retain their normal relocation behavior.
  }
#endif
  MacroAssemblerSpecific::movePtr(imm, dest);
}

#ifndef JS_CODEGEN_RISCV64
inline void MacroAssembler::loadPtr(AbsoluteAddress addr, Register dest) {
#  ifdef ENABLE_JS_AOT
  if (MOZ_UNLIKELY(isAOT())) {
    uintptr_t val = uintptr_t(addr.addr);
    if (auto slot = aotTable().findSlot(val)) {
      if (IsAOTLinkSlot(*slot)) {
        emitAOTLinkLoad(*slot, dest);
      } else {
        emitAOTSlotLoad(*slot, dest);
        MacroAssemblerSpecific::loadPtr(Address(dest, 0), dest);
      }
      return;
    }
    AOT_CRASH_ON_UNKNOWN_PTR("loadPtr(AbsoluteAddress)", val);
  }
#  endif
  MacroAssemblerSpecific::loadPtr(addr, dest);
}
#endif

inline void MacroAssembler::storePtr(ImmPtr imm, const Address& address) {
#ifdef ENABLE_JS_AOT
  if (MOZ_UNLIKELY(isAOT())) {
    uintptr_t val = uintptr_t(imm.value);
    if (auto slot = aotTable().findSlot(val)) {
      ScratchRegisterScope scratch(*this);
      if (IsAOTLinkSlot(*slot)) {
        emitAOTLinkAddress(*slot, scratch);
      } else {
        emitAOTSlotLoad(*slot, scratch);
      }
      MacroAssemblerSpecific::storePtr(scratch, address);
      return;
    }
    AOT_CRASH_ON_UNKNOWN_PTR("storePtr(ImmPtr, Address)", val);
  }
#endif
  MacroAssemblerSpecific::storePtr(imm, address);
}

#ifndef JS_CODEGEN_RISCV64
inline void MacroAssembler::storePtr(Register src, AbsoluteAddress address) {
#  ifdef ENABLE_JS_AOT
  if (MOZ_UNLIKELY(isAOT())) {
    uintptr_t val = uintptr_t(address.addr);
    if (auto slot = aotTable().findSlot(val)) {
      ScratchRegisterScope scratch(*this);
      if (IsAOTLinkSlot(*slot)) {
        emitAOTLinkAddress(*slot, scratch);
      } else {
        emitAOTSlotLoad(*slot, scratch);
      }
      MacroAssemblerSpecific::storePtr(src, Address(scratch, 0));
      return;
    }
    AOT_CRASH_ON_UNKNOWN_PTR("storePtr(Register, AbsoluteAddress)", val);
  }
#  endif
  MacroAssemblerSpecific::storePtr(src, address);
}
#endif

inline void MacroAssembler::jump(TrampolinePtr code) {
#ifdef ENABLE_JS_AOT
  if (MOZ_UNLIKELY(isAOT())) {
    uintptr_t val = uintptr_t(code.value);
    if (auto slot = aotTable().findSlot(val)) {
      ScratchRegisterScope scratch(*this);
      emitAOTSlotLoad(*slot, scratch);
      MacroAssemblerSpecific::jump(scratch);
      return;
    }
    AOT_CRASH_ON_UNKNOWN_PTR("jump(TrampolinePtr)", val);
  }
#endif
  MacroAssemblerSpecific::jump(code);
}

#ifdef ENABLE_JS_AOT
#  undef AOT_CRASH_ON_UNKNOWN_PTR
#endif

}  // namespace js::jit

#endif  // jit_AOTMacroAssembler_inl_h
