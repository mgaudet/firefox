/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOTABIFns_inl_h
#define jit_AOTABIFns_inl_h

#ifdef ENABLE_JS_AOT

#  include <iterator>
#  include <stdint.h>

#  include "jit/AOT.h"
#  include "js/experimental/TypedData.h"

#  include "jit/ABIFunctionList-inl.h"

namespace js::jit {

// Only the shape of the list matters here, so the entries are never named or
// evaluated. That keeps the declarations behind them out of this header.
inline constexpr bool kAOTABIFnLinkable[] = {
#  define AOT_ABIFN(fp) true,
#  define AOT_ABIFN_NOLINK(fp) false,
#  define AOT_ABIFN_TYPED(fp, ...) true,
#  include "jit/AOTABIFns.tbl"
#  undef AOT_ABIFN_TYPED
#  undef AOT_ABIFN_NOLINK
#  undef AOT_ABIFN
};

inline constexpr uint32_t kAOTABIFnCount = std::size(kAOTABIFnLinkable);

static_assert(kAOTABIFnCount <= AOTMaxABIFunctions, "raise AOTMaxABIFunctions");

// Companion to the named slot hash, covering the ABI function half of the
// numbering. Link sites name an ABI function by index, so reordering or
// renaming an entry has to invalidate an image recorded against the old order.
constexpr uint32_t AOTABIFnTableHash() {
  const char* const entries[] = {
#  define AOT_ABIFN(fp) #fp,
#  define AOT_ABIFN_NOLINK(fp) "!" #fp,
#  define AOT_ABIFN_TYPED(fp, ...) #fp "->" #__VA_ARGS__,
#  include "jit/AOTABIFns.tbl"
#  undef AOT_ABIFN_TYPED
#  undef AOT_ABIFN_NOLINK
#  undef AOT_ABIFN
  };
  uint32_t h = 2166136261u;
  auto mix = [&h](uint8_t b) { h = (h ^ b) * 16777619u; };
  for (const char* e : entries) {
    for (; *e; e++) {
      mix(uint8_t(*e));
    }
    mix('|');
  }
  for (int i = 0; i < 4; i++) {
    mix(uint8_t(kAOTABIFnCount >> (8 * i)));
  }
  return h;
}

// Identifies everything an image's link sites bind against.
constexpr uint32_t AOTImageLinkHash() {
  uint32_t h = AOTSlotTableHash();
  uint32_t abi = AOTABIFnTableHash();
  for (int i = 0; i < 4; i++) {
    h = (h ^ uint8_t(abi >> (8 * i))) * 16777619u;
  }
  return h;
}

}  // namespace js::jit

#endif  // ENABLE_JS_AOT

#endif  // jit_AOTABIFns_inl_h
