// The TERMINAL iterator helpers (ECMA-262 27.1.4.1): `reduce`, `toArray`,
// `forEach`, `some`, `every` and `find`.
//
// The seam against iterator_helpers.cpp is what the member RETURNS. Everything
// there is lazy and answers an iterator; everything here drives the iteration to
// an end and answers a value, so none of it needs an Iterator Helper object, a
// state slot, or a resumption — one loop each, and the loop finishes before the
// member does.
//
// What they do share is the three-line rule that a helper must not leave an
// iterator suspended, and here it has two shapes:
//
//   - the SHORT-CIRCUITING three (`some`, `every`, `find`) close the iterator on
//     the element that decided the answer. That close is a normal completion, so
//     an error the iterator's `return` raises is the member's error — unlike the
//     close after a callback throws, where the callback's error wins.
//   - a non-callable callback closes the iterator too (each member's step 3.b is
//     `IteratorClose(O, error)`), before a single element has been read.
//
// Both live in iterator_helpers_internal.h so that the two files perform the
// protocol identically.

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/iterator.h"
#include "runtime/iterator_helpers_internal.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime::iterator_helpers {

namespace {

// The opening of every member here: the receiver must be an object, the
// callback must be callable (closing the receiver if it is not), and `next` is
// then read once. False means an exception is pending and the member is over.
//
// `wantsCallback` is false for `toArray`, the one member with no callback at
// all — written as a parameter rather than a second function because the other
// five steps are identical and a copy of them is how the two would drift.
bool openTerminal(Rooted<Value>& self, Rooted<Value>& callback, bool wantsCallback,
                  const char* member, Rooted<Value>& nextOut) {
    if (!self.get().isObject()) {
        rtThrowTypeError("Iterator.prototype." + std::string(member) +
                         " called on a value that is not an object");
        return false;
    }
    if (wantsCallback && !isCallable(callback.get())) {
        closeAndThrowTypeError(self, "Iterator.prototype." + std::string(member) +
                                         " requires a function argument");
        return false;
    }
    return getIteratorDirect(self, member, nextOut);
}

// One callback call with (value, counter). On a throw the iterator is closed and
// the callback's own error re-thrown (IfAbruptCloseIterator), which is why every
// caller can simply test `rtExceptionPending` afterwards and return.
bool callBack(Rooted<Value>& fn, Rooted<Value>& iter, Rooted<Value>& value, double counter,
              Rooted<Value>& out) {
    Value block[2] = {value.get(), Value::fromDouble(counter)};
    out.set(fn.get().asObject<FunctionHeader>()->call(Value::fromUndefined(), 2, block));
    if (rtExceptionPending()) {
        closeAfterThrow(iter);
        return false;
    }
    return true;
}

// The shared body of `some`, `every` and `find`: they differ in which truthiness
// stops the walk and in what the stop answers, and in nothing else.
enum class Predicate { Some, Every, Find };

uint64_t runPredicate(uint64_t thisBits, uint32_t argc, const uint64_t* argv, Predicate which) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    Rooted<Value> fn{args[0]};
    const char* member =
        which == Predicate::Some ? "some" : (which == Predicate::Every ? "every" : "find");
    Rooted<Value> next;
    if (!openTerminal(self, fn, /*wantsCallback=*/true, member, next)) {
        return Value::fromUndefined().rawBits();
    }
    double counter = 0.0;
    for (;;) {
        Rooted<Value> value;
        const Step step = stepIterator(self, next, value);
        if (step == Step::Threw) return Value::fromUndefined().rawBits();
        if (step == Step::Done) {
            // The exhausted answers: nothing satisfied `some`, everything
            // satisfied `every`, and `find` found nothing.
            switch (which) {
                case Predicate::Some: return Value::fromBool(false).rawBits();
                case Predicate::Every: return Value::fromBool(true).rawBits();
                default: return Value::fromUndefined().rawBits();
            }
        }
        Rooted<Value> verdict;
        if (!callBack(fn, self, value, counter, verdict)) {
            return Value::fromUndefined().rawBits();
        }
        counter += 1.0;
        const bool truthy = bronze_truthy(verdict.get().rawBits());
        const bool stop = which == Predicate::Every ? !truthy : truthy;
        if (!stop) continue;
        // The decision closes the iterator as a NORMAL completion: an error
        // `return` raises here really is this member's error, because there is
        // no earlier throw for it to be suppressed in favour of.
        closeIterator(self, /*suppress=*/false);
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        switch (which) {
            case Predicate::Some: return Value::fromBool(true).rawBits();
            case Predicate::Every: return Value::fromBool(false).rawBits();
            default: return value.get().rawBits();
        }
    }
}

}  // namespace

// 27.1.4.1.10 Iterator.prototype.reduce(reducer [, initialValue]).
uint64_t iteratorReduce(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    Rooted<Value> reducer{args[0]};
    Rooted<Value> next;
    if (!openTerminal(self, reducer, /*wantsCallback=*/true, "reduce", next)) {
        return Value::fromUndefined().rawBits();
    }
    Rooted<Value> accumulator;
    double counter = 0.0;
    // Step 5: with no initial value the FIRST element is the accumulator, and an
    // empty iterator is then a TypeError rather than `undefined` — the same rule
    // `Array.prototype.reduce` has, and the reason the two arms differ by one in
    // the index the reducer is passed.
    //
    // `argc` and not `args[1].isUndefined()`: `reduce(f, undefined)` PASSED an
    // initial value, and the two must not be confused.
    if (args.count() < 2) {
        const Step step = stepIterator(self, next, accumulator);
        if (step == Step::Threw) return Value::fromUndefined().rawBits();
        if (step == Step::Done) {
            return rtThrowTypeError(
                       "Iterator.prototype.reduce of an empty iterator with no initial value")
                .rawBits();
        }
        counter = 1.0;
    } else {
        accumulator.set(args[1]);
    }
    for (;;) {
        Rooted<Value> value;
        const Step step = stepIterator(self, next, value);
        if (step == Step::Threw) return Value::fromUndefined().rawBits();
        if (step == Step::Done) return accumulator.get().rawBits();
        // Three arguments, not two: the reducer is called with (accumulator,
        // value, index), so `callBack`'s two-argument shape does not fit.
        Value block[3] = {accumulator.get(), value.get(), Value::fromDouble(counter)};
        Rooted<Value> result{
            reducer.get().asObject<FunctionHeader>()->call(Value::fromUndefined(), 3, block)};
        if (rtExceptionPending()) {
            closeAfterThrow(self);
            return Value::fromUndefined().rawBits();
        }
        accumulator.set(result.get());
        counter += 1.0;
    }
}

// 27.1.4.1.13 Iterator.prototype.toArray(). The one member here with no
// callback, and so the one that cannot fail in the middle for a reason of its
// own — every error it can raise comes out of the iterator.
uint64_t iteratorToArray(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    Rooted<Value> none;
    Rooted<Value> next;
    if (!openTerminal(self, none, /*wantsCallback=*/false, "toArray", next)) {
        return Value::fromUndefined().rawBits();
    }
    // Length ZERO and grown by the appends: `bronze_create_array(n)` SETS the
    // length, so a capacity guess would leave trailing `undefined` elements.
    Rooted<Value> out{Value(bronze_create_array(0))};
    for (;;) {
        Rooted<Value> value;
        const Step step = stepIterator(self, next, value);
        if (step == Step::Threw) return Value::fromUndefined().rawBits();
        if (step == Step::Done) return out.get().rawBits();
        const uint32_t at = out.get().asObject<ArrayHeader>()->length;
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at, value);
    }
}

// 27.1.4.1.9 Iterator.prototype.forEach(procedure).
uint64_t iteratorForEach(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    Rooted<Value> fn{args[0]};
    Rooted<Value> next;
    if (!openTerminal(self, fn, /*wantsCallback=*/true, "forEach", next)) {
        return Value::fromUndefined().rawBits();
    }
    double counter = 0.0;
    for (;;) {
        Rooted<Value> value;
        const Step step = stepIterator(self, next, value);
        if (step != Step::Produced) return Value::fromUndefined().rawBits();
        Rooted<Value> ignored;
        if (!callBack(fn, self, value, counter, ignored)) {
            return Value::fromUndefined().rawBits();
        }
        counter += 1.0;
    }
}

uint64_t iteratorSome(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return runPredicate(thisBits, argc, argv, Predicate::Some);
}

uint64_t iteratorEvery(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return runPredicate(thisBits, argc, argv, Predicate::Every);
}

uint64_t iteratorFind(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return runPredicate(thisBits, argc, argv, Predicate::Find);
}

}  // namespace bronze::runtime::iterator_helpers
