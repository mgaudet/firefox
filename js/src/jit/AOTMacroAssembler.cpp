/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineFrame.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "vm/JSContext.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

#ifdef ENABLE_JS_AOT

void MacroAssembler::emitAOTLoadTableBase(Register dest) {
  // Load the indirection table address from the stub frame when one is active.
  // Otherwise load it from the baseline frame.
  if (inAOTStubFrame_) {
    MacroAssemblerSpecific::loadPtr(
        Address(FramePointer, BaselineStubFrameLayout::AOTTableOffsetFromFP),
        dest);
  } else {
    MacroAssemblerSpecific::loadPtr(
        Address(FramePointer, BaselineFrame::reverseOffsetOfAOTTableBase()),
        dest);
  }
}

void MacroAssembler::emitAOTSlotLoad(AOTSlot slot, Register dest) {
  emitAOTLoadTableBase(dest);
  int32_t slotOff = int32_t(AOTIndirectionTable::offsetOfSlot(slot));
  MacroAssemblerSpecific::loadPtr(Address(dest, slotOff), dest);
}

// The rip relative encoders return the offset just past the instruction, so
// its displacement occupies the four bytes before that.
static uint32_t DisplacementOffset(CodeOffset afterInstruction) {
  MOZ_ASSERT(afterInstruction.offset() >= sizeof(int32_t));
  return uint32_t(afterInstruction.offset()) - sizeof(int32_t);
}

void MacroAssembler::emitAOTLinkAddress(AOTSlot slot, Register dest) {
  MOZ_ASSERT(IsAOTLinkSlot(slot));
  CodeOffset off = Assembler::leaRipRelative(dest);
  propagateOOM(aotLinkSites_.append(
      AOTLinkSite{DisplacementOffset(off), uint32_t(slot)}));
}

void MacroAssembler::emitAOTLinkLoad(AOTSlot slot, Register dest) {
  MOZ_ASSERT(IsAOTLinkSlot(slot));
  CodeOffset off = Assembler::loadRipRelativeInt64(dest);
  propagateOOM(aotLinkSites_.append(
      AOTLinkSite{DisplacementOffset(off), uint32_t(slot)}));
}

static AOTSlot PreBarrierSlotForMIRType(MIRType type) {
  switch (type) {
    case MIRType::Value:
      return AOTSlot::PreBarrier_Value;
    case MIRType::String:
      return AOTSlot::PreBarrier_String;
    case MIRType::Object:
      return AOTSlot::PreBarrier_Object;
    case MIRType::Shape:
      return AOTSlot::PreBarrier_Shape;
    case MIRType::WasmAnyRef:
      return AOTSlot::PreBarrier_WasmAnyRef;
    default:
      MOZ_CRASH("Unexpected MIRType for pre-barrier");
  }
}

void MacroAssembler::callPreBarrierAOT(MIRType type, Register scratch) {
  MOZ_ASSERT(isAOT());
  MOZ_ASSERT(scratch != PreBarrierReg);
  emitAOTSlotLoad(PreBarrierSlotForMIRType(type), scratch);
  call(scratch);
}

void MacroAssembler::loadZoneForAOT(Register dest) {
  MOZ_ASSERT(isAOT());
  loadJSContext(dest);
  MacroAssemblerSpecific::loadPtr(Address(dest, JSContext::offsetOfZone()),
                                  dest);
}

void MacroAssembler::emitAOTDispatch(Register opcodeReg, Register tableReg) {
  if (isAOT()) {
    // AOT dispatch tables store int32 offsets relative to the table base.
    BaseIndex entry(tableReg, opcodeReg, TimesFour);
    load32SignExtendToPtr(entry, opcodeReg);
    addPtr(tableReg, opcodeReg);
    jump(opcodeReg);
  } else {
    BaseIndex pointer(tableReg, opcodeReg, ScalePointer);
    branchToComputedAddress(pointer);
  }
}

size_t MacroAssembler::aotDispatchTableEntrySize() const {
  return isAOT() ? sizeof(int32_t) : sizeof(uintptr_t);
}

void MacroAssembler::emitAOTStoreFrameTableBase(Register passReg,
                                                Register scratch,
                                                const Address& dst) {
  if (isAOT()) {
    // The entry preamble places the indirection table address in the register
    // used to initialize this frame.
    storePtr(passReg, dst);
    return;
  }
  // All baseline frames store the indirection table address so static inline
  // cache stubs can access it.
  movePtr(ImmPtr(runtime()->jitRuntime()->aotIndirectionTable().baseAddress()),
          scratch);
  storePtr(scratch, dst);
}

void MacroAssembler::emitAOTCopyFrameTableBaseFromCaller(Register scratch) {
  // When resuming a generator, copy the indirection table address from the
  // caller's saved baseline frame into the reconstructed frame.
  loadPtr(Address(FramePointer, 0), scratch);
  loadPtr(Address(scratch, BaselineFrame::reverseOffsetOfAOTTableBase()),
          scratch);
  storePtr(scratch,
           Address(FramePointer, BaselineFrame::reverseOffsetOfAOTTableBase()));
}

#endif  // ENABLE_JS_AOT
