// `yield*` at the runtime's end of it: ONE resumption forwarded to the iterator
// a delegating yield opened (ECMA-262 27.5.3.7 steps 5.a, 5.b and 5.c).
//
// The LOOP those steps sit in is compiled code and cannot be here. A delegation
// yields, and a yield in bronze is a RETURN from the resume function — so the
// repeat of 27.5.3.7 step 5 is a cycle through the generator object's `next`,
// which is to say through the caller. What is left for the runtime is the part
// of each step that is not control flow: which method of the inner iterator
// this resumption calls, whether it has one at all, and the three checks
// 7.4.4/7.4.5 need a real object for.
//
// Its own unit rather than more of iterator.cpp, because it is a different
// question about the same record. That file decides how a value is WALKED —
// the cursor kinds and the protocol kind, opened once and stepped; this one
// decides how a walk already opened answers a resumption it did not ask for.
//
// The cursor kinds (an array, a string, a typed array, a Map, a Set) have no
// iterator object in bronze, so they have no `throw` and no `return` to look
// up. That is not an approximation here: ECMA-262 gives %ArrayIteratorPrototype%
// and %StringIteratorPrototype% neither method either, so "there is no method"
// is the answer a spec engine gives too, and the paths below that fire on a
// missing method are the ones a delegation to an array really takes.

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/generator.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/iterator.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/value.h"

namespace bronze::runtime {
namespace {

bool isCallable(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function;
}

// 7.4.1 CreateIterResultObject, in the spec's field order — `value` then
// `done`, which is the order `Object.keys` of one reports.
//
// Reached only for a cursor kind. A protocol iterator's result object is the
// one IT built, and the delegation forwards that object by identity (27.5.3.8
// GeneratorYield takes the iterator result and hands it on unchanged), so
// building a second one would be observable as a different object.
Value cursorResult(Rooted<Value>& value, bool done) {
    Rooted<Value> out{Value(bronze_create_object())};
    Rooted<Value> vk{rtMakeString("value")};
    out.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), vk, value);
    Rooted<Value> dk{rtMakeString("done")};
    Rooted<Value> dv{Value::fromBool(done)};
    out.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), dk, dv);
    return out.get();
}

// 7.3.11 GetMethod: `undefined` and `null` both answer "no method", anything
// else that is not callable is a TypeError, and only a callable comes back.
//
// The distinction matters more here than anywhere else GetMethod is used:
// 27.5.3.7 5.b.iii closes the iterator before reporting a MISSING `throw`, and
// does not close it for a `throw` that is present and uncallable — that one is
// GetMethod's own abrupt completion, propagated by the `?` in step 5.b.i.
Value getMethod(Rooted<Value>& obj, const char* name) {
    Rooted<Value> key{rtMakeString(name)};
    Rooted<Value> found{Value(bronze_elem_get(obj.get().rawBits(), key.get().rawBits()))};
    if (rtExceptionPending()) return Value::fromUndefined();
    if (found.get().isUndefined() || found.get().isNull()) return Value::fromUndefined();
    if (!isCallable(found.get())) {
        rtThrowTypeError(std::string("the delegated iterator's `") + name +
                         "` is not a function");
        return Value::fromUndefined();
    }
    return found.get();
}

// The three "if innerResult is not an Object, throw a TypeError" steps
// (5.a.iii, 5.b.ii.3 and 5.c.vi), which are one rule: a delegation reads `done`
// and `value` off whatever came back, and a primitive has neither.
Value requireResultObject(Value result) {
    if (result.isObject()) return result;
    return rtThrowTypeError("the delegated iterator's result is not an object");
}

// One call into the inner iterator, with the received value as its single
// argument. `method` and `receiver` are already rooted by the caller; the
// argument array is not, and does not need to be — `FunctionHeader::call`
// publishes it as the callee's argument frame, which is a root source.
Value callWithReceived(Rooted<Value>& method, Rooted<Value>& receiver, Rooted<Value>& sent) {
    Value args[1] = {sent.get()};
    Value result = method.get().asObject<FunctionHeader>()->call(receiver.get(), 1, args);
    if (rtExceptionPending()) return Value::fromUndefined();
    return requireResultObject(result);
}

IterRecordHeader* recordOf(Value recVal) {
    if (!recVal.isObject() ||
        recVal.asObject<HeapObjectHeader>()->flags != IterRecordHeader::kFlags) {
        fatal("internal: iter.delegate on a value that is not an iteration record");
    }
    return recVal.asObject<IterRecordHeader>();
}

// 5.a: a NORMAL resumption calls the iterator's `next` with the value the outer
// `next(v)` supplied. The `next` method is the one GetIterator read at open
// time (7.4.2 step 3 reads it once), which is why an iterator that replaces its
// own `next` mid-delegation does not change what this calls.
Value delegateNext(Rooted<Value>& recRoot, Rooted<Value>& sent) {
    IterRecordHeader* rec = recordOf(recRoot.get());
    if (rec->kindOf() != IterRecordHeader::Protocol) {
        const bool more = bronze_iter_step(recRoot.get().rawBits());
        if (rtExceptionPending()) return Value::fromUndefined();
        Rooted<Value> produced{more ? Value(bronze_iter_value(recRoot.get().rawBits()))
                                    : Value::fromUndefined()};
        return cursorResult(produced, !more);
    }
    Rooted<Value> nextFn{rec->nextFn};
    Rooted<Value> iterObj{rec->target};
    return callWithReceived(nextFn, iterObj, sent);
}

// 5.b: a THROW resumption looks for the inner iterator's `throw`. The clause
// worth reading twice is 5.b.iii — an iterator WITHOUT one is closed first and
// the TypeError raised second, so an inner `return` method still runs before
// the protocol violation is reported. Close-then-throw, not throw-instead-of-
// close.
Value delegateThrow(Rooted<Value>& recRoot, Rooted<Value>& sent) {
    IterRecordHeader* rec = recordOf(recRoot.get());
    const bool protocol = rec->kindOf() == IterRecordHeader::Protocol;
    Rooted<Value> iterObj{protocol ? rec->target : Value::fromUndefined()};
    Rooted<Value> thrower{protocol ? getMethod(iterObj, "throw") : Value::fromUndefined()};
    if (rtExceptionPending()) return Value::fromUndefined();
    if (!isCallable(thrower.get())) {
        bronze_iter_close(recRoot.get().rawBits(), /*suppress=*/false);
        // 7.4.9 propagates an error the `return` method raised, and it replaces
        // the TypeError below rather than being replaced by it: step 5 of
        // IteratorClose returns that completion before 5.b.iii.5 is reached.
        if (rtExceptionPending()) return Value::fromUndefined();
        return rtThrowTypeError(
            "the delegated iterator has no `throw` method, so the exception cannot be forwarded "
            "into it");
    }
    return callWithReceived(thrower, iterObj, sent);
}

// 5.c: a RETURN resumption looks for the inner iterator's `return`. Two answers
// and not one: an iterator with no `return` lets the return completion pass
// straight through the delegation (5.c.iii), which no result object can say —
// so `undefined` says it, and every other path here produces an Object or
// throws.
Value delegateReturn(Rooted<Value>& recRoot, Rooted<Value>& sent) {
    IterRecordHeader* rec = recordOf(recRoot.get());
    const bool protocol = rec->kindOf() == IterRecordHeader::Protocol;
    Rooted<Value> iterObj{protocol ? rec->target : Value::fromUndefined()};
    Rooted<Value> returner{protocol ? getMethod(iterObj, "return") : Value::fromUndefined()};
    if (rtExceptionPending()) return Value::fromUndefined();
    if (!isCallable(returner.get())) return Value::fromUndefined();
    return callWithReceived(returner, iterObj, sent);
}

}  // namespace

extern "C" {

uint64_t bronze_iter_delegate(uint64_t recBits, uint64_t modeBits, uint64_t sentBits) {
    Rooted<Value> recRoot{Value(recBits)};
    Rooted<Value> sent{Value(sentBits)};
    // The resumption kind arrives as a NUMBER rather than a machine integer,
    // because the generated code that supplies it took it from the resume
    // function's `__mode` parameter, which the uniform calling convention
    // delivers as a boxed value like every other argument.
    const auto mode = static_cast<uint32_t>(Value(modeBits).asNumber());
    switch (mode) {
        case GeneratorResumeMode::Next: return delegateNext(recRoot, sent).rawBits();
        case GeneratorResumeMode::Throw: return delegateThrow(recRoot, sent).rawBits();
        case GeneratorResumeMode::Return: return delegateReturn(recRoot, sent).rawBits();
        default: break;
    }
    fatal("internal: iter.delegate with an unknown resumption mode");
}

}  // extern "C"

}  // namespace bronze::runtime
