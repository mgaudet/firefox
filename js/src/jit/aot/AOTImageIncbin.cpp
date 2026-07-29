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

#define AOT_IMAGE_SITE(slot)                                  \
  asm(AOT_IMAGE_PUSH ".long %c0 - . - 4" AOT_IMAGE_POP ::"s"( \
      AOTLinkSym<AOTSlot(slot)>::value));

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

#undef AOT_IMAGE_SITE
#undef AOT_IMAGE_CHUNK
#undef AOT_IMAGE_SLOT_TABLE_HASH
#undef AOT_IMAGE_POP
#undef AOT_IMAGE_PUSH

}  // namespace
}  // namespace js::jit
