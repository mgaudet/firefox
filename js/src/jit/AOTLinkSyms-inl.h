/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOTLinkSyms_inl_h
#define jit_AOTLinkSyms_inl_h

#ifdef ENABLE_JS_AOT

#  include <iterator>

#  include "builtin/DataViewObject.h"
#  include "builtin/MapObject.h"
#  include "builtin/WeakMapObject.h"
#  include "builtin/WeakSetObject.h"
#  include "jit/AOT.h"
#  include "proxy/DeadObjectProxy.h"
#  include "vm/ArgumentsObject.h"
#  include "vm/ArrayBufferObject.h"
#  include "vm/ArrayObject.h"
#  include "vm/BoundFunctionObject.h"
#  include "vm/DateObject.h"
#  include "vm/EnvironmentObject.h"
#  include "vm/GeneratorObject.h"
#  include "vm/Iteration.h"
#  include "vm/JSFunction.h"
#  include "vm/PlainObject.h"
#  include "vm/SharedArrayObject.h"
#  include "vm/TypedArrayObject.h"

#  include "jit/AOTABIFns-inl.h"

namespace js::jit {

// Restates each link slot's address as a constant expression so the assembler
// can print it as a symbol reference. Only the image shim instantiates these.
// Naming a slot that has no specialization is a compile error, which is what
// keeps the generated relocation list tied to the slot tables.
template <AOTSlot S>
struct AOTLinkSym;

// ABI functions are numbered by position rather than named, so the index has
// to be derived from the same single pass over the list that the runtime table
// population makes. The counter is captured here and subtracted back out, and
// only this header expands the list this way, so the specializations below are
// identical in every translation unit that could see them.
constexpr uint32_t kAOTABIFnCounterBase = __COUNTER__;

#  define AOT_ABIFN(fp)                                                    \
    template <>                                                            \
    struct AOTLinkSym<AOTSlotForABIFn(__COUNTER__ - kAOTABIFnCounterBase - \
                                      1)> {                                \
      static constexpr auto value = (fp);                                  \
    };
#  define AOT_ABIFN_TYPED(fp, ...)                                         \
    template <>                                                            \
    struct AOTLinkSym<AOTSlotForABIFn(__COUNTER__ - kAOTABIFnCounterBase - \
                                      1)> {                                \
      static constexpr auto value = static_cast<__VA_ARGS__>(fp);          \
    };
// Consumes the index without defining a symbol, so a link site that names one
// of these fails to compile instead of binding to the wrong entry.
#  define AOT_ABIFN_RESERVE(counter) \
    static_assert(!kAOTABIFnLinkable[(counter) - kAOTABIFnCounterBase - 1]);
#  define AOT_ABIFN_NOLINK(fp) AOT_ABIFN_RESERVE(__COUNTER__)
#  include "jit/AOTABIFns.tbl"
#  undef AOT_ABIFN_NOLINK
#  undef AOT_ABIFN_RESERVE
#  undef AOT_ABIFN_TYPED
#  undef AOT_ABIFN

static_assert(__COUNTER__ - kAOTABIFnCounterBase - 1 == kAOTABIFnCount,
              "ABI function list expanded inconsistently");

#  define AOT_SLOT(name, expr)
#  define AOT_ATOM_SLOT(name, expr)
#  define AOT_LINK_SLOT(name, expr)         \
    template <>                             \
    struct AOTLinkSym<AOTSlot::name> {      \
      static constexpr auto value = (expr); \
    };
#  include "jit/AOTSlots.tbl"
#  undef AOT_LINK_SLOT
#  undef AOT_ATOM_SLOT
#  undef AOT_SLOT

}  // namespace js::jit

#endif  // ENABLE_JS_AOT

#endif  // jit_AOTLinkSyms_inl_h
