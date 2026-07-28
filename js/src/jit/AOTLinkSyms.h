/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AOTLinkSyms_h
#define jit_AOTLinkSyms_h

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

namespace js::jit {

// Restates each link slot's address as a constant expression so the assembler
// can print it as a symbol reference. Only the image shim instantiates these.
// Naming a slot that has no specialization is a compile error, which is what
// keeps the generated relocation list tied to AOTSlots.tbl.
template <AOTSlot S>
struct AOTLinkSym;

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

#endif  // jit_AOTLinkSyms_h
