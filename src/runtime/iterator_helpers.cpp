// The LAZY iterator helpers (ECMA-262 27.1.4.1): `map`, `filter`, `take`,
// `drop` and `flatMap`, the Iterator Helper object they all return, `Iterator`
// itself and `Iterator.from`.
//
// Laziness is the whole design. `it.map(f)` runs `f` on nothing: it allocates
// one small object holding the underlying iterator, its `next`, the callback and
// a counter, and every one of those five helpers is that same object with a
// different `Kind`. The work happens in `helperNext`, one element at a time, so
// `it.map(f).take(2)` calls `f` twice however long `it` is — which is the reason
// the proposal exists and the thing a helper that materialized an array would
// silently lose.
//
// Two obligations run through all of it, and both are about the UNDERLYING
// iterator rather than the helper:
//
//   - a helper that stops early CLOSES what it was reading. `take` closes when
//     its count runs out, and every callback that throws closes before the
//     throw leaves (7.4.12 IfAbruptCloseIterator). An iterator left suspended is
//     a `finally` in a generator that never runs.
//   - the protocol is performed GENERICALLY. `next` is a property read off the
//     receiver and `done`/`value` are property reads off its result, because
//     27.1.4.1 is defined over any object with a `next` — `Iterator.from({next(){}})`
//     is a supported program.
//
// The methods live on %IteratorPrototype% (iterator.cpp installs them there), so
// every built-in iterator bronze has inherits all eleven without any of them
// knowing this file exists.

#include <cmath>
#include <iterator>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/heap.h"
#include "runtime/iterator.h"
#include "runtime/iterator_helpers_internal.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace iterator_helpers {

bool isCallable(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function;
}

namespace {

// A named property of an arbitrary value, through the general element path —
// which is what makes it work on a proxy, on a receiver with a getter, and on a
// foreign iterator a host handed in. The key is built into its own root first:
// making it a sibling argument of the receiver read would leave the order of two
// allocating expressions to the compiler.
Value genericGet(Rooted<Value>& obj, const char* key) {
    Rooted<Value> keyRoot{rtMakeString(key)};
    return Value(bronze_elem_get(obj.get().rawBits(), keyRoot.get().rawBits()));
}

}  // namespace

Value iterResult(Rooted<Value>& value, bool done) {
    Rooted<Value> out{Value(bronze_create_object())};
    Rooted<Value> vk{rtMakeString("value")};
    out.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), vk, value);
    Rooted<Value> dk{rtMakeString("done")};
    Rooted<Value> dv{Value::fromBool(done)};
    out.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), dk, dv);
    return out.get();
}

bool getIteratorDirect(Rooted<Value>& obj, const char* member, Rooted<Value>& nextOut) {
    if (!obj.get().isObject()) {
        rtThrowTypeError("Iterator.prototype." + std::string(member) +
                         " called on a value that is not an object");
        return false;
    }
    nextOut.set(genericGet(obj, "next"));
    return !rtExceptionPending();
}

Step stepIterator(Rooted<Value>& iter, Rooted<Value>& next, Rooted<Value>& out) {
    if (!isCallable(next.get())) {
        rtThrowTypeError("the iterator has no `next` method");
        return Step::Threw;
    }
    Rooted<Value> result{next.get().asObject<FunctionHeader>()->call(iter.get(), 0, nullptr)};
    if (rtExceptionPending()) return Step::Threw;
    // 7.4.4 IteratorNext step 3: a result that is not an object is a TypeError,
    // and NOT an exhausted iterator — a `next` returning `undefined` is a bug in
    // the iterator and reading `done` off `undefined` would hide it.
    if (!result.get().isObject()) {
        rtThrowTypeError("the iterator result is not an object");
        return Step::Threw;
    }
    Rooted<Value> done{genericGet(result, "done")};
    if (rtExceptionPending()) return Step::Threw;
    if (bronze_truthy(done.get().rawBits())) return Step::Done;
    out.set(genericGet(result, "value"));
    if (rtExceptionPending()) return Step::Threw;
    return Step::Produced;
}

void closeIterator(Rooted<Value>& iter, bool suppress) {
    if (!iter.get().isObject()) return;
    Rooted<Value> ret{genericGet(iter, "return")};
    if (rtExceptionPending()) {
        if (suppress) rtClearException();
        return;
    }
    // 7.4.11 step 4: an iterator with no `return` closes by doing nothing.
    if (!isCallable(ret.get())) return;
    ret.get().asObject<FunctionHeader>()->call(iter.get(), 0, nullptr);
    if (suppress && rtExceptionPending()) rtClearException();
}

void closeAfterThrow(Rooted<Value>& iter) {
    Rooted<Value> pending{Value(bronze_exception_cell)};
    rtClearException();
    closeIterator(iter, /*suppress=*/true);
    rtThrow(pending.get());
}

void closeAndThrowTypeError(Rooted<Value>& iter, const std::string& message) {
    closeIterator(iter, /*suppress=*/true);
    rtThrowTypeError(message);
}

void closeAndThrowRangeError(Rooted<Value>& iter, const std::string& message) {
    closeIterator(iter, /*suppress=*/true);
    rtThrowRangeError(message);
}

bool getIteratorFlattenable(Rooted<Value>& value, bool allowStringPrimitive, const char* member,
                            Rooted<Value>& iterOut, Rooted<Value>& nextOut) {
    if (!value.get().isObject()) {
        // Step 1: a primitive is refused, with the single exception
        // `Iterator.from` makes for a String — whose characters really are an
        // iteration a program can mean.
        if (!allowStringPrimitive || !value.get().isString()) {
            rtThrowTypeError(std::string(member) + " requires an object" +
                             (allowStringPrimitive ? " or a string" : "") + ", not " +
                             rtIterableKindName(value.get()));
            return false;
        }
    }
    Rooted<Value> key{rtIteratorKey()};
    Rooted<Value> method{Value(bronze_elem_get(value.get().rawBits(), key.get().rawBits()))};
    if (rtExceptionPending()) return false;
    if (method.get().isUndefined() || method.get().isNull()) {
        // Step 3.b: no @@iterator means the value IS the iterator. This is the
        // arm that accepts `{next(){...}}`, and the reason the operation is
        // called "flattenable" rather than "iterable".
        iterOut.set(value.get());
    } else {
        if (!isCallable(method.get())) {
            rtThrowTypeError(std::string(member) + ": the value's Symbol.iterator is not a "
                                                   "function");
            return false;
        }
        iterOut.set(method.get().asObject<FunctionHeader>()->call(value.get(), 0, nullptr));
        if (rtExceptionPending()) return false;
    }
    if (!iterOut.get().isObject()) {
        rtThrowTypeError(std::string(member) + ": the iterator is not an object");
        return false;
    }
    return getIteratorDirect(iterOut, member, nextOut);
}

}  // namespace iterator_helpers

namespace {

using iterator_helpers::closeAfterThrow;
using iterator_helpers::closeAndThrowRangeError;
using iterator_helpers::closeAndThrowTypeError;
using iterator_helpers::closeIterator;
using iterator_helpers::getIteratorDirect;
using iterator_helpers::getIteratorFlattenable;
using iterator_helpers::isCallable;
using iterator_helpers::iterResult;
using iterator_helpers::Step;
using iterator_helpers::stepIterator;

// Which helper an Iterator Helper object is. One object, five behaviours, and
// the reason 27.1.4.2 is a single prototype rather than five.
enum HelperKind : uint32_t { KindMap = 0, KindFilter, KindTake, KindDrop, KindFlatMap };

// [[GeneratorState]] as 27.1.4.1 uses it for a helper: `Running` exists so that
// a callback which reaches back into the helper it is running inside is the
// TypeError the spec names rather than a re-entrant walk of the same cursor.
enum HelperState : uint32_t { StateActive = 0, StateRunning, StateDone };

Value readSlot(Rooted<Value>& obj, uint32_t slot) {
    return obj.get().asObject<ObjectHeader>()->internalSlot(slot);
}

void writeSlot(Rooted<Value>& obj, uint32_t slot, Value val) {
    obj.get().asObject<ObjectHeader>()->setInternalSlot(slot, val);
}

uint32_t slotNumber(Rooted<Value>& obj, uint32_t slot) {
    return static_cast<uint32_t>(readSlot(obj, slot).asNumber());
}

const char* helperName(uint32_t kind) {
    switch (kind) {
        case KindMap: return "map";
        case KindFilter: return "filter";
        case KindTake: return "take";
        case KindDrop: return "drop";
        default: return "flatMap";
    }
}

// A fresh Iterator Helper over `receiver`, with `fn` in the closure slot — the
// callback for `map`/`filter`/`flatMap`, the remaining count for
// `take`/`drop`. Every caller has already performed GetIteratorDirect, so the
// `next` it read is passed in rather than read again: 27.1.4.1 reads it ONCE,
// and an iterator that swaps its own `next` mid-walk must not change what the
// helper calls.
Value makeHelper(Rooted<Value>& receiver, Rooted<Value>& next, Rooted<Value>& fn, uint32_t kind) {
    Rooted<Value> helper{rtNewIteratorObject(IteratorProto::Helper)};
    // Written after the allocation, through the roots, and each `writeSlot`
    // re-derives the object — the pattern every iterator kind here uses.
    writeSlot(helper, IteratorHelperSlot::Kind, Value::fromDouble(static_cast<double>(kind)));
    writeSlot(helper, IteratorHelperSlot::Iterated, receiver.get());
    writeSlot(helper, IteratorHelperSlot::NextMethod, next.get());
    writeSlot(helper, IteratorHelperSlot::Fn, fn.get());
    writeSlot(helper, IteratorHelperSlot::Counter, Value::fromDouble(0.0));
    writeSlot(helper, IteratorHelperSlot::Inner, Value::fromUndefined());
    writeSlot(helper, IteratorHelperSlot::InnerNext, Value::fromUndefined());
    writeSlot(helper, IteratorHelperSlot::State, Value::fromDouble(StateActive));
    return helper.get();
}

// Call the helper's callback with (value, counter), advancing the counter. The
// counter is read and written through the helper's root, so a callback that
// itself allocates cannot leave it stale.
//
// Returns false with the underlying iterator ALREADY CLOSED and the callback's
// exception re-thrown — IfAbruptCloseIterator, which every callback in 27.1.4.1
// is wrapped in.
bool callWithCounter(Rooted<Value>& helper, Rooted<Value>& iter, Rooted<Value>& value,
                     Rooted<Value>& out) {
    Rooted<Value> fn{readSlot(helper, IteratorHelperSlot::Fn)};
    const double counter = readSlot(helper, IteratorHelperSlot::Counter).asNumber();
    writeSlot(helper, IteratorHelperSlot::Counter, Value::fromDouble(counter + 1.0));
    Value block[2] = {value.get(), Value::fromDouble(counter)};
    out.set(fn.get().asObject<FunctionHeader>()->call(Value::fromUndefined(), 2, block));
    if (rtExceptionPending()) {
        closeAfterThrow(iter);
        return false;
    }
    return true;
}

// One element of a `map`, `filter` or `flatMap`, or one step of a `take` /
// `drop`: the per-kind body of `next`, with the state machine around it left to
// the caller.
Step advanceHelper(Rooted<Value>& helper, Rooted<Value>& out) {
    Rooted<Value> iter{readSlot(helper, IteratorHelperSlot::Iterated)};
    Rooted<Value> next{readSlot(helper, IteratorHelperSlot::NextMethod)};
    const uint32_t kind = slotNumber(helper, IteratorHelperSlot::Kind);

    switch (kind) {
        case KindMap: {
            const Step step = stepIterator(iter, next, out);
            if (step != Step::Produced) return step;
            Rooted<Value> mapped;
            if (!callWithCounter(helper, iter, out, mapped)) return Step::Threw;
            out.set(mapped.get());
            return Step::Produced;
        }
        case KindFilter: {
            for (;;) {
                const Step step = stepIterator(iter, next, out);
                if (step != Step::Produced) return step;
                Rooted<Value> keep;
                if (!callWithCounter(helper, iter, out, keep)) return Step::Threw;
                if (bronze_truthy(keep.get().rawBits())) return Step::Produced;
            }
        }
        case KindTake: {
            const double remaining = readSlot(helper, IteratorHelperSlot::Fn).asNumber();
            if (!(remaining > 0.0)) {
                // 27.1.4.1.11 step 6.b.i: the count running out CLOSES the
                // underlying iterator rather than merely abandoning it, which is
                // what lets `gen().take(1)` run the generator's `finally`.
                closeIterator(iter, /*suppress=*/false);
                if (rtExceptionPending()) return Step::Threw;
                return Step::Done;
            }
            // Infinity - 1 is Infinity, which is exactly what `take(Infinity)`
            // has to mean.
            writeSlot(helper, IteratorHelperSlot::Fn, Value::fromDouble(remaining - 1.0));
            return stepIterator(iter, next, out);
        }
        case KindDrop: {
            double remaining = readSlot(helper, IteratorHelperSlot::Fn).asNumber();
            while (remaining > 0.0) {
                const Step step = stepIterator(iter, next, out);
                if (step != Step::Produced) return step;
                remaining -= 1.0;
                // Written per element, not once at the end: a `next` that throws
                // half way through the drop must not make the NEXT call start
                // the drop over.
                writeSlot(helper, IteratorHelperSlot::Fn, Value::fromDouble(remaining));
            }
            return stepIterator(iter, next, out);
        }
        default: {
            for (;;) {
                Rooted<Value> inner{readSlot(helper, IteratorHelperSlot::Inner)};
                if (inner.get().isObject()) {
                    Rooted<Value> innerNext{readSlot(helper, IteratorHelperSlot::InnerNext)};
                    const Step step = stepIterator(inner, innerNext, out);
                    if (step == Step::Produced) return Step::Produced;
                    if (step == Step::Threw) {
                        // 27.1.4.1.7 step 6.d.iv.2: an inner iterator's throw
                        // closes the OUTER one. The inner is already finished by
                        // definition of having thrown.
                        closeAfterThrow(iter);
                        return Step::Threw;
                    }
                    writeSlot(helper, IteratorHelperSlot::Inner, Value::fromUndefined());
                    writeSlot(helper, IteratorHelperSlot::InnerNext, Value::fromUndefined());
                }
                const Step step = stepIterator(iter, next, out);
                if (step != Step::Produced) return step;
                Rooted<Value> mapped;
                if (!callWithCounter(helper, iter, out, mapped)) return Step::Threw;
                Rooted<Value> innerIter;
                Rooted<Value> innerNext;
                // reject-primitives: a STRING the mapper returned is a
                // TypeError, not an iteration of its characters. `flatMap` is
                // the one place the language makes that choice explicitly, and
                // it is the choice that turns `names.flatMap(n => n)` into an
                // error instead of a stream of letters.
                if (!getIteratorFlattenable(mapped, /*allowStringPrimitive=*/false,
                                            "Iterator.prototype.flatMap", innerIter, innerNext)) {
                    closeAfterThrow(iter);
                    return Step::Threw;
                }
                writeSlot(helper, IteratorHelperSlot::Inner, innerIter.get());
                writeSlot(helper, IteratorHelperSlot::InnerNext, innerNext.get());
            }
        }
    }
}

// ---- %IteratorHelperPrototype% (27.1.4.2) -----------------------------------

uint64_t helperNext(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    if (!rtIsIteratorObject(self.get(), IteratorProto::Helper)) {
        return rtThrowTypeError(
                   "Iterator Helper.prototype.next called on a value that is not an iterator "
                   "helper")
            .rawBits();
    }
    const uint32_t state = slotNumber(self, IteratorHelperSlot::State);
    if (state == StateRunning) {
        return rtThrowTypeError("this iterator helper is already running").rawBits();
    }
    Rooted<Value> produced;
    if (state == StateDone) return iterResult(produced, true).rawBits();

    writeSlot(self, IteratorHelperSlot::State, Value::fromDouble(StateRunning));
    const Step step = advanceHelper(self, produced);
    if (step != Step::Produced) {
        // Done AND thrown both complete the helper for good: 27.1.4.1's
        // generator bodies have no step after either, so a second `next` must
        // answer `{value: undefined, done: true}` and never touch the underlying
        // iterator again.
        writeSlot(self, IteratorHelperSlot::State, Value::fromDouble(StateDone));
        if (step == Step::Threw) return Value::fromUndefined().rawBits();
        Rooted<Value> none;
        return iterResult(none, true).rawBits();
    }
    writeSlot(self, IteratorHelperSlot::State, Value::fromDouble(StateActive));
    return iterResult(produced, false).rawBits();
}

// 27.1.4.2.2. `return` on a helper closes what the helper was reading and marks
// it finished — the `break` out of a `for-of` over `it.map(f)` reaches here, and
// it is the only reason the underlying iterator gets closed in that case.
uint64_t helperReturn(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    if (!rtIsIteratorObject(self.get(), IteratorProto::Helper)) {
        return rtThrowTypeError(
                   "Iterator Helper.prototype.return called on a value that is not an iterator "
                   "helper")
            .rawBits();
    }
    Rooted<Value> none;
    if (slotNumber(self, IteratorHelperSlot::State) == StateDone) {
        return iterResult(none, true).rawBits();
    }
    writeSlot(self, IteratorHelperSlot::State, Value::fromDouble(StateDone));
    Rooted<Value> iter{readSlot(self, IteratorHelperSlot::Iterated)};
    // A `flatMap` abandoned mid-inner closes BOTH, innermost first, which is the
    // order the nested generator bodies of 27.1.4.1.7 would unwind in.
    Rooted<Value> inner{readSlot(self, IteratorHelperSlot::Inner)};
    if (inner.get().isObject()) closeIterator(inner, /*suppress=*/false);
    closeIterator(iter, /*suppress=*/false);
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    return iterResult(none, true).rawBits();
}

// ---- %WrapForValidIteratorPrototype% (27.1.3.2.1) ---------------------------

// The wrapper `Iterator.from` builds for an iterator that does NOT already
// inherit %Iterator.prototype%. It adds nothing but the chain: `next` forwards
// to the iterator it holds, so the point of the object is that the helpers above
// become reachable on a foreign iterator without touching it.
uint64_t wrapNext(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    if (!rtIsIteratorObject(self.get(), IteratorProto::Wrap)) {
        return rtThrowTypeError("next called on a value that is not an Iterator.from wrapper")
            .rawBits();
    }
    Rooted<Value> iter{readSlot(self, IteratorHelperSlot::Iterated)};
    Rooted<Value> next{readSlot(self, IteratorHelperSlot::NextMethod)};
    if (!isCallable(next.get())) {
        return rtThrowTypeError("the wrapped iterator has no `next` method").rawBits();
    }
    // The result object is passed through UNCHANGED — 27.1.3.2.1.1 is
    // `Call(next, iterated)` and nothing more, so a `next` returning extra
    // properties keeps them.
    return next.get().asObject<FunctionHeader>()->call(iter.get(), 0, nullptr).rawBits();
}

uint64_t wrapReturn(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    if (!rtIsIteratorObject(self.get(), IteratorProto::Wrap)) {
        return rtThrowTypeError("return called on a value that is not an Iterator.from wrapper")
            .rawBits();
    }
    Rooted<Value> iter{readSlot(self, IteratorHelperSlot::Iterated)};
    Rooted<Value> key{rtMakeString("return")};
    Rooted<Value> ret{Value(bronze_elem_get(iter.get().rawBits(), key.get().rawBits()))};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    // 27.1.3.2.1.2 step 5: a wrapped iterator with no `return` answers a done
    // result rather than failing — the wrapper must not invent a method.
    if (!isCallable(ret.get())) {
        Rooted<Value> none;
        return iterResult(none, true).rawBits();
    }
    return ret.get().asObject<FunctionHeader>()->call(iter.get(), 0, nullptr).rawBits();
}

// ---- the five lazy helpers (27.1.4.1) ---------------------------------------

// The shape every one of `map`, `filter` and `flatMap` shares: the receiver must
// be an object, the argument must be callable, and the helper is then built over
// the `next` read from the receiver. A non-callable argument CLOSES the receiver
// (step 3.b is `IteratorClose(O, error)`), which is why the check is here and
// not in the caller.
uint64_t makeCallbackHelper(uint64_t thisBits, uint32_t argc, const uint64_t* argv,
                            uint32_t kind) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    // The three checks in the spec's order (steps 2, 3, 4): the receiver, then
    // the callback, then `next`. The order is observable — a non-callable
    // argument closes the receiver WITHOUT its `next` having been read, so an
    // iterator with a `next` getter must not see that getter run.
    if (!self.get().isObject()) {
        return rtThrowTypeError("Iterator.prototype." + std::string(helperName(kind)) +
                                " called on a value that is not an object")
            .rawBits();
    }
    Rooted<Value> fn{args[0]};
    if (!isCallable(fn.get())) {
        closeAndThrowTypeError(self, "Iterator.prototype." + std::string(helperName(kind)) +
                                         " requires a function argument");
        return Value::fromUndefined().rawBits();
    }
    Rooted<Value> next;
    if (!getIteratorDirect(self, helperName(kind), next)) {
        return Value::fromUndefined().rawBits();
    }
    return makeHelper(self, next, fn, kind).rawBits();
}

uint64_t iteratorMap(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return makeCallbackHelper(thisBits, argc, argv, KindMap);
}

uint64_t iteratorFilter(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return makeCallbackHelper(thisBits, argc, argv, KindFilter);
}

uint64_t iteratorFlatMap(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return makeCallbackHelper(thisBits, argc, argv, KindFlatMap);
}

// `take` and `drop` share their whole argument protocol: ToNumber, then NaN and
// a negative count are each a RangeError that closes the receiver first. The
// count is kept as a DOUBLE and not an integer, because `Infinity` is a legal
// argument to both and is the only way to spell "all of them".
uint64_t makeCountHelper(uint64_t thisBits, uint32_t argc, const uint64_t* argv, uint32_t kind) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!self.get().isObject()) {
        return rtThrowTypeError("Iterator.prototype." + std::string(helperName(kind)) +
                                " called on a value that is not an object")
            .rawBits();
    }
    const double raw = rtToNumber(args[0]);
    if (rtExceptionPending()) {
        // Step 4's IfAbruptCloseIterator: a `valueOf` that threw still closes
        // the iterator this helper would have read.
        closeAfterThrow(self);
        return Value::fromUndefined().rawBits();
    }
    if (std::isnan(raw)) {
        closeAndThrowRangeError(self, "Iterator.prototype." + std::string(helperName(kind)) +
                                          " requires a number, not NaN");
        return Value::fromUndefined().rawBits();
    }
    // ToIntegerOrInfinity: truncate toward zero, and leave an infinity alone.
    const double count = std::isinf(raw) ? raw : std::trunc(raw);
    if (count < 0.0) {
        closeAndThrowRangeError(self, "Iterator.prototype." + std::string(helperName(kind)) +
                                          " requires a non-negative count");
        return Value::fromUndefined().rawBits();
    }
    Rooted<Value> next;
    if (!getIteratorDirect(self, helperName(kind), next)) {
        return Value::fromUndefined().rawBits();
    }
    Rooted<Value> limit{Value::fromDouble(count)};
    return makeHelper(self, next, limit, kind).rawBits();
}

uint64_t iteratorTake(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return makeCountHelper(thisBits, argc, argv, KindTake);
}

uint64_t iteratorDrop(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return makeCountHelper(thisBits, argc, argv, KindDrop);
}

// ---- `Iterator` and `Iterator.from` (27.1.3) --------------------------------

Value g_iteratorCtor = Value::fromUndefined();

// 27.1.3.1.1 Iterator.from(O).
uint64_t iteratorFrom(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> source{args[0]};
    Rooted<Value> iter;
    Rooted<Value> next;
    if (!getIteratorFlattenable(source, /*allowStringPrimitive=*/true, "Iterator.from", iter,
                                next)) {
        return Value::fromUndefined().rawBits();
    }
    // Step 3: an iterator that ALREADY inherits %Iterator.prototype% is returned
    // unchanged. That is what makes `Iterator.from` idempotent and keeps it from
    // wrapping a generator — which already has the helpers — in a second object
    // whose identity a program would then see in `===`.
    Rooted<Value> ctor{rtIteratorConstructor("Iterator")};
    if (rtOrdinaryHasInstance(ctor.get(), iter.get())) return iter.get().rawBits();

    Rooted<Value> wrapper{rtNewIteratorObject(IteratorProto::Wrap)};
    writeSlot(wrapper, IteratorHelperSlot::Kind, Value::fromDouble(0.0));
    writeSlot(wrapper, IteratorHelperSlot::Iterated, iter.get());
    writeSlot(wrapper, IteratorHelperSlot::NextMethod, next.get());
    writeSlot(wrapper, IteratorHelperSlot::Fn, Value::fromUndefined());
    writeSlot(wrapper, IteratorHelperSlot::Counter, Value::fromDouble(0.0));
    writeSlot(wrapper, IteratorHelperSlot::Inner, Value::fromUndefined());
    writeSlot(wrapper, IteratorHelperSlot::InnerNext, Value::fromUndefined());
    writeSlot(wrapper, IteratorHelperSlot::State, Value::fromDouble(StateActive));
    return wrapper.get().rawBits();
}

// 27.1.3.1: `Iterator` is ABSTRACT. Calling it, and `new Iterator()`, are both
// a TypeError — the constructor exists so that subclasses have a base and so
// that `x instanceof Iterator` can ask whether something is an iterator at all.
//
// A SUBCLASS's `super()` is the one call that must succeed, and the receiver is
// what tells it apart. The spec's test is on NewTarget, which the uniform
// calling convention cannot see through a `super()` — bronze_super_call enters
// the body with the derived instance and nothing else — so the equivalent
// question is asked of the instance's own [[Prototype]]:
//
//   `new Iterator()` builds its instance from THIS constructor's shape, so its
//   prototype is %Iterator.prototype% and it is refused; `class D extends
//   Iterator` builds one from D's, so its prototype is `D.prototype` and it is
//   allowed.
//
// The two answers coincide with 27.1.3.1's for every program that does not
// deliberately re-point a subclass's `prototype` AT %Iterator.prototype% —
// which would be a class asking to be the abstract one.
uint64_t iteratorConstructorBody(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    if (self.get().isObject() &&
        self.get().asObject<HeapObjectHeader>()->flags == BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        const ObjectHeader* obj = self.get().asObject<ObjectHeader>();
        const Shape* root = obj->shape ? obj->shape->root : nullptr;
        if (root && root->prototype.isObject() &&
            root->prototype.rawBits() != rtIteratorSharedPrototype().rawBits()) {
            return self.get().rawBits();
        }
    }
    return rtThrowTypeError(
               "Iterator is abstract and cannot be constructed directly (27.1.3.1); it exists "
               "to be extended and to answer `instanceof`")
        .rawBits();
}

}  // namespace

// ECMA-262 27.1.4.1, in the spec's own order. Every arity is the spec's
// `length`, except that a variadic member would take 0 — none here is variadic.
void rtInstallIteratorHelpers(Rooted<Value>& proto) {
    const NativeMethod methods[] = {
        {"map", iteratorMap, 1},
        {"filter", iteratorFilter, 1},
        {"take", iteratorTake, 1},
        {"drop", iteratorDrop, 1},
        {"flatMap", iteratorFlatMap, 1},
        {"reduce", iterator_helpers::iteratorReduce, 1},
        {"toArray", iterator_helpers::iteratorToArray, 0},
        {"forEach", iterator_helpers::iteratorForEach, 1},
        {"some", iterator_helpers::iteratorSome, 1},
        {"every", iterator_helpers::iteratorEvery, 1},
        {"find", iterator_helpers::iteratorFind, 1},
    };
    rtDefineMethods(proto, methods, std::size(methods));
}

void rtInstallIteratorHelperPrototype(Rooted<Value>& proto) {
    const NativeMethod methods[] = {
        {"next", helperNext, 0},
        {"return", helperReturn, 0},
    };
    rtDefineMethods(proto, methods, std::size(methods));
}

void rtInstallIteratorWrapPrototype(Rooted<Value>& proto) {
    const NativeMethod methods[] = {
        {"next", wrapNext, 0},
        {"return", wrapReturn, 0},
    };
    rtDefineMethods(proto, methods, std::size(methods));
}

Value rtIteratorConstructor(const std::string& name) {
    if (name != "Iterator") return Value::fromUndefined();
    if (g_iteratorCtor.isObject()) return g_iteratorCtor;
    Rooted<Value> fn{rtNativeFunction(iteratorConstructorBody, 0)};
    // Published BEFORE the decoration below, because `iteratorConstructorBody`
    // compares NewTarget against it and a subclass could be constructed while
    // the properties are still being installed.
    g_iteratorCtor = fn.get();
    rtHeap().add_permanent_root(&g_iteratorCtor);

    // `Iterator.prototype` IS %IteratorPrototype% (27.1.3.2) — the same object
    // every built-in iterator already inherits, not a fresh one. Setting the
    // slot directly is what makes `Iterator.prototype.map` and
    // `[].values().map` one function, and what makes `instanceof` work through
    // the ordinary chain walk with nothing of its own.
    {
        Rooted<Value> proto{rtIteratorSharedPrototype()};
        FunctionHeader* live = fn.get().asObject<FunctionHeader>();
        live->prototype = proto.get();
        // Set together with the prototype, because `rtEnsureFunctionPrototype`
        // tests both and would otherwise mint a fresh empty object over it.
        live->instance_shape = rtRootShapeForPrototype(proto.get());
    }

    rtEnsureFunctionProperties(fn);
    Rooted<Value> props{fn.get().asObject<FunctionHeader>()->properties};
    Rooted<Value> key{rtMakeString("from")};
    Rooted<Value> from{rtNativeFunction(iteratorFrom, 1)};
    props.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, from, nullptr,
                                                  /*enumerable=*/false, /*defineOwn=*/true);
    g_iteratorCtor = fn.get();
    return g_iteratorCtor;
}

}  // namespace bronze::runtime
