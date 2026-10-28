#pragma once

// `arr.push(v)` and `arr.shift()` inline, for a method call whose receiver
// inference typed a dense JS Array (il.h `kMethodRecvArray`): the append or
// the front removal that bronze_array_push / bronze_array_shift make on their
// own fast path, done at the site, with the call through the method cache and
// the native as its miss.
//
// What makes skipping the method lookup sound is the same fact the method
// cache's EXOTIC Array entry stands on (runtime/rt_method_call.cpp): an Array
// with no named-property box finds `push` and `shift` in the immutable C
// method table, because nothing can shadow them — an own `push`, a subclass's
// prototype, `Object.setPrototypeOf` and the integrity levels all give the
// array a box — and nothing can replace them, because decorating
// `Array.prototype` is a hard error by construction (rt_prop_write.cpp). So
// the guard is the receiver's kind and an empty box, and no epoch or
// builtin-intact cell is needed: there is no state in which the guard holds
// and the method is anything else.

#include "il_ast.h"
#include <brass/mir/builder.hpp>
#include <functional>

namespace il2mir {

class IlLowering;

enum class ArrayInline { None, Push, Shift };

// Which inline arm this method call takes, if any: a hinted receiver, `push`
// with one argument or `shift` with none, in an image build's code that runs
// more than once.
ArrayInline array_method_inline(IlLowering* lowering, const BronzeInstruction& inst);

// The arm in front of `slow` (the site's ordinary call). `val` is the pushed
// value's bits, unused by shift. Returns the call's result as Value bits.
Value* lower_array_method_inline(IlLowering* lowering, Builder& b, ArrayInline which, Value* recv, Value* val,
                                 const std::function<Value*(Builder&)>& slow);

}  // namespace il2mir
