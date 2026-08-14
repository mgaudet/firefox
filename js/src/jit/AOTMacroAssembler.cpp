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
#  ifdef JS_AOT_STUB_FRAME_HAS_TABLE_SLOT
    MacroAssemblerSpecific::loadPtr(
        Address(FramePointer, BaselineStubFrameLayout::AOTTableOffsetFromFP),
        dest);
#  else
    // No slot in the stub frame, so go through the caller. [FP] is the saved
    // frame pointer of the BaselineJS frame that entered the stub, which is
    // where the prologue stored the table address. Same two hops
    // emitAOTCopyFrameTableBaseFromCaller relies on.
    MacroAssemblerSpecific::loadPtr(Address(FramePointer, 0), dest);
    MacroAssemblerSpecific::loadPtr(
        Address(dest, BaselineFrame::reverseOffsetOfAOTTableBase()), dest);
#  endif
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

void MacroAssembler::emitAOTSlotCall(AOTSlot slot, Register scratch) {
#  if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
  emitAOTLoadTableBase(scratch);
  call(Address(scratch, int32_t(AOTIndirectionTable::offsetOfSlot(slot))));
#  else
  emitAOTSlotLoad(slot, scratch);
  call(scratch);
#  endif
}

void MacroAssembler::emitAOTSlotJump(AOTSlot slot, Register scratch) {
#  if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
  emitAOTLoadTableBase(scratch);
  MacroAssemblerSpecific::jump(
      Address(scratch, int32_t(AOTIndirectionTable::offsetOfSlot(slot))));
#  else
  emitAOTSlotLoad(slot, scratch);
  MacroAssemblerSpecific::jump(scratch);
#  endif
}

// Link sites name a symbol whose address the static linker resolves, so the
// recorded instruction leaves a hole the image shim re-emits as a relocation.
// The hole's shape is the instruction encoding's, which is why this is per
// backend. Backends without an implementation report no link slots from
// IsAOTLinkSlot, so every caller takes the indirection table path instead and
// these are unreachable.
#  ifdef JS_CODEGEN_X64

// The encoders return the offset just past the instruction, so its
// displacement occupies the four bytes before that.
static uint32_t DisplacementOffset(CodeOffset afterInstruction) {
  MOZ_ASSERT(afterInstruction.offset() >= sizeof(int32_t));
  return uint32_t(afterInstruction.offset()) - sizeof(int32_t);
}

void MacroAssembler::emitAOTLinkAddress(AOTSlot slot, Register dest) {
  MOZ_ASSERT(IsAOTLinkSlot(slot));
  CodeOffset off = Assembler::leaRipRelative(dest);
  propagateOOM(aotLinkSites_.append(
      AOTLinkSite{DisplacementOffset(off), uint32_t(slot),
                  uint8_t(AOTLinkKind::Address), 0, sizeof(int32_t)}));
}

void MacroAssembler::emitAOTLinkCall(AOTSlot slot) {
  MOZ_ASSERT(IsAOTLinkSlot(slot));
  CodeOffset off = Assembler::callWithPatch();
  propagateOOM(aotLinkSites_.append(
      AOTLinkSite{DisplacementOffset(off), uint32_t(slot),
                  uint8_t(AOTLinkKind::Call), 0, sizeof(int32_t)}));
}

void MacroAssembler::emitAOTLinkLoad(AOTSlot slot, Register dest) {
  MOZ_ASSERT(IsAOTLinkSlot(slot));
  CodeOffset off = Assembler::loadRipRelativeInt64(dest);
  propagateOOM(aotLinkSites_.append(
      AOTLinkSite{DisplacementOffset(off), uint32_t(slot),
                  uint8_t(AOTLinkKind::Load64), 0, sizeof(int32_t)}));
}

#  elif defined(JS_CODEGEN_ARM64)

// The shim emits whole instructions here, so the recorder reserves exactly the
// space they will occupy and leaves it zero. Pools and alignment nops are
// forbidden across the reservation: anything the assembler slipped in would
// shift every later branch target relative to what the shim produces.
void MacroAssembler::reserveAOTLinkSite(AOTSlot slot, AOTLinkKind kind,
                                        Register dest, uint32_t instructions) {
  MOZ_ASSERT(IsAOTLinkSlot(slot));
  // The shim spells the register into its asm text, so encoding 31 would name
  // xzr/sp rather than a destination.
  MOZ_ASSERT_IF(kind != AOTLinkKind::Call, dest.code() != Registers::xzr);
  AutoForbidPoolsAndNops afp(this, instructions);
  uint32_t offset = currentOffset();
  for (uint32_t i = 0; i < instructions; i++) {
    writeInt32Data(0);
  }
  MOZ_ASSERT(currentOffset() - offset == instructions * sizeof(uint32_t));
  propagateOOM(aotLinkSites_.append(
      AOTLinkSite{offset, uint32_t(slot), uint8_t(kind), uint8_t(dest.code()),
                  uint16_t(instructions * sizeof(uint32_t))}));
}

// adrp + add, reaching +-4GB.
void MacroAssembler::emitAOTLinkAddress(AOTSlot slot, Register dest) {
  reserveAOTLinkSite(slot, AOTLinkKind::Address, dest, 2);
}

// A single bl, reaching +-128MB. The linker inserts a range extension thunk
// beyond that. The stack pointer sync that MacroAssembler::call performs has to
// happen before the reservation, not inside it, because the shim replaces
// exactly the reserved words.
void MacroAssembler::emitAOTLinkCall(AOTSlot slot) {
  syncStackPtr();
  reserveAOTLinkSite(slot, AOTLinkKind::Call, Register{Registers::x0}, 1);
}

// adrp + ldr.
void MacroAssembler::emitAOTLinkLoad(AOTSlot slot, Register dest) {
  reserveAOTLinkSite(slot, AOTLinkKind::Load64, dest, 2);
}

#  else

void MacroAssembler::emitAOTLinkAddress(AOTSlot slot, Register dest) {
  MOZ_CRASH("AOT link slots are not implemented for this backend");
}

void MacroAssembler::emitAOTLinkCall(AOTSlot slot) {
  MOZ_CRASH("AOT link slots are not implemented for this backend");
}

void MacroAssembler::emitAOTLinkLoad(AOTSlot slot, Register dest) {
  MOZ_CRASH("AOT link slots are not implemented for this backend");
}

#  endif

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
  emitAOTSlotCall(PreBarrierSlotForMIRType(type), scratch);
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
