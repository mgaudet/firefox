/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Embeds the generated AOT image and exports symbols for its byte range. When
 * no recorded image is available, the build embeds an empty valid image so
 * runtime generation remains available.
 *
 * The image arrives split around its link sites. Recorded code reaches some
 * addresses with a rip relative instruction whose four byte displacement was
 * left zero, and the generated list names the slot each of those fields refers
 * to. Emitting a relocation there instead of the recorded zeros lets this
 * build's static linker supply the displacement, so the runtime does not pay
 * an indirection table load to reach a value that is already fixed at link
 * time. Everything between sites is copied in verbatim.
 *
 * Emitting the fragments from one function body keeps them contiguous and in
 * order. The function is never called; only its assembly matters.
 */

#include "jit/AOTABIFns-inl.h"
#include "jit/AOTLinkSyms-inl.h"

extern "C" {
extern const uint8_t aot_image_start[];
extern const uint8_t aot_image_end[];
}

namespace js::jit {
namespace {

#define AOT_IMAGE_PUSH ".pushsection .text.aot,\"ax\",@progbits\n\t"
#define AOT_IMAGE_POP "\n\t.popsection"

// Each fragment restores the previous section so the enclosing function's own
// instructions cannot land inside the image.

// Zero means the build has no recorded image to check.
#define AOT_IMAGE_SLOT_TABLE_HASH(hash)                      \
  static_assert((hash) == 0 || (hash) == AOTImageLinkHash(), \
                "AOT image was recorded against a different slot table");

#define AOT_IMAGE_CHUNK(offset, length)                  \
  asm(AOT_IMAGE_PUSH ".incbin \"AOTImage.inc\"," #offset \
                     "," #length AOT_IMAGE_POP);

// Marks the true end of the emitted image, before the padding that aligns
// aot_image_end. AOTImage::embedded checks the span against the size recorded
// in the header: a site that assembles to a different length than the recorder
// reserved would shift every later branch target, with no other symptom until
// the code runs.
//
// An assembler-level ".if . - aot_image_start != bytes" would catch this at
// build time instead, and does work for a single chunk, but the expression
// stops being absolute once the image is split across many chunks, so it
// cannot be relied on.
#define AOT_IMAGE_TOTAL_SIZE(bytes)                              \
  asm(AOT_IMAGE_PUSH ".globl aot_image_emitted_end\n\t"          \
                     ".type aot_image_emitted_end, @object\n"   \
                     "aot_image_emitted_end:" AOT_IMAGE_POP);

#if defined(__x86_64__)

// The recorded instruction is kept; only its rip relative displacement is
// replaced. The destination register is already encoded in the retained
// opcode, so the register operand is unused here.
#  define AOT_IMAGE_SITE_CALL(slot, reg)                        \
    asm(AOT_IMAGE_PUSH ".long %c0 - . - 4" AOT_IMAGE_POP ::"s"( \
        AOTLinkSym<AOTSlot(slot)>::value));
#  define AOT_IMAGE_SITE_ADDR(slot, reg) AOT_IMAGE_SITE_CALL(slot, reg)
#  define AOT_IMAGE_SITE_LOAD(slot, reg) AOT_IMAGE_SITE_CALL(slot, reg)

#elif defined(__aarch64__)

// No displacement field exists to patch, so the shim emits whole instructions
// and the recorder reserved exactly this much space. Note the constraint: the
// x86 spelling "s" is rejected by clang on aarch64; "S" is the symbolic-address
// constraint here.
#  define AOT_IMAGE_SITE_CALL(slot, reg)                    \
    asm(AOT_IMAGE_PUSH "bl %c0" AOT_IMAGE_POP ::"S"(        \
        AOTLinkSym<AOTSlot(slot)>::value));
#  define AOT_IMAGE_SITE_ADDR(slot, reg)                                  \
    asm(AOT_IMAGE_PUSH "adrp x" #reg ", %c0\n\t"                          \
                       "add x" #reg ", x" #reg ", :lo12:%c0"             \
        AOT_IMAGE_POP ::"S"(AOTLinkSym<AOTSlot(slot)>::value));
#  define AOT_IMAGE_SITE_LOAD(slot, reg)                                  \
    asm(AOT_IMAGE_PUSH "adrp x" #reg ", %c0\n\t"                          \
                       "ldr x" #reg ", [x" #reg ", :lo12:%c0]"           \
        AOT_IMAGE_POP ::"S"(AOTLinkSym<AOTSlot(slot)>::value));

#else
#  error "No AOT image shim for this target"
#endif

__attribute__((used)) void EmbedAOTImage() {
  asm(AOT_IMAGE_PUSH
      ".balign 4096\n\t"
      ".globl aot_image_start\n\t"
      ".type aot_image_start, @object\n"
      "aot_image_start:" AOT_IMAGE_POP);

#include "jit/aot/AOTImageRelocs.inc"

  asm(AOT_IMAGE_PUSH
      ".balign 4096\n\t"
      ".globl aot_image_end\n\t"
      ".type aot_image_end, @object\n"
      "aot_image_end:\n\t"
      ".size aot_image_start, aot_image_end - aot_image_start" AOT_IMAGE_POP);
}

#undef AOT_IMAGE_SITE_LOAD
#undef AOT_IMAGE_SITE_ADDR
#undef AOT_IMAGE_SITE_CALL
#undef AOT_IMAGE_TOTAL_SIZE
#undef AOT_IMAGE_CHUNK
#undef AOT_IMAGE_SLOT_TABLE_HASH
#undef AOT_IMAGE_POP
#undef AOT_IMAGE_PUSH

}  // namespace
}  // namespace js::jit
