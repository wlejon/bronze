#pragma once

#include <cstddef>

// BEFORE rt_internal.h, and the order is load-bearing: rt_internal.h names
// `struct FunctionHeader` in a parameter list, which DECLARES a second,
// incomplete `bronze::runtime::FunctionHeader` unless the real one in fn.h is
// already visible. Every other builtin gets this by alphabetical luck.
#include "runtime/fn.h"

#include "runtime/rt_internal.h"
#include "runtime/value.h"

// What the two halves of the Date builtin share. `builtin_date.cpp` owns the
// object — the brand, the constructor, the prototype and the statics — and
// `builtin_date_accessors.cpp` owns the thirty-odd field getters and setters,
// which are one algorithm apiece and would otherwise bury it.

namespace bronze::runtime {

// The internal slots (ECMA-262 6.1.7.2) a Date instance carries.
//
// `TimeValue` is [[DateValue]]. `Brand` is bronze's and holds a symbol the
// runtime mints once and never hands to a program — the witness that makes this
// object a Date. It is needed because the (slot count, slot type) pair the
// primitive wrappers brand themselves with is already taken: a Number object is
// a plain object with one internal slot holding a number, which is exactly what
// a one-slot Date would be, and `console.log(date)` would have printed
// `[Number: 1577836800000]`.
//
// Branding on a slot rather than on the prototype also keeps [[DateValue]]
// where the specification puts it: `Object.setPrototypeOf(d, null)` does not
// stop `d` being a Date, and `Object.create(Date.prototype)` does not start.
namespace DateSlot {
enum : uint32_t { TimeValue, Brand, kCount };
}

// Is this value a Date instance — the brand, in the one place that owns it.
bool rtIsDateObject(Value v);

// A fresh Date object holding `t`. ALLOCATES.
Value rtMakeDateObject(double t);

// 21.4.4.1 thisTimeValue with its TypeError already raised on failure: false
// means a receiver without [[DateValue]], and the caller returns `undefined`
// into its slot while the exception cell carries the error. `method` names the
// member in the message, because "called on an incompatible receiver" without
// one sends a reader nowhere.
bool rtDateThisTimeValue(Value self, const char* method, double& out);

// Write [[DateValue]]. The receiver must already have passed the brand check.
void rtDateSetTimeValue(Value self, double t);

// The getter/setter half of `Date.prototype`, as a table the assembling half
// installs. One list, so a member cannot be implemented and un-registered.
const NativeMethod* rtDateAccessorMethods(size_t& count);

}  // namespace bronze::runtime
