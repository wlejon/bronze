// %GeneratorPrototype% (ECMA-262 27.5.1) and the generator object's three
// methods. What is here is [[GeneratorState]] and nothing else: the WALK lives
// in compiled code — one resume function per generator, closed over the frame —
// and this file decides only whether it may be entered and what a resumption
// that never reaches it answers.
//
// The split is the spec's own. 27.5.3.2 and 27.5.3.3 are almost entirely a
// state machine over [[GeneratorState]] with one step that says "resume the
// execution context"; that one step is the call below and everything around it
// is here. Keeping it here rather than in the generated dispatch also puts the
// two rules that a program can observe from OUTSIDE the body in one place:
//
//   - 27.5.3.2 step 2: resuming a generator that is already executing is a
//     TypeError. It is not a state the body can be asked about, because the
//     body is on the stack when it happens.
//   - a completed generator stays completed. `next()` answers
//     `{ value: undefined, done: true }` for ever, `return(v)` answers
//     `{ value: v, done: true }`, and `throw(e)` raises `e` — none of which
//     re-enters the body at all.
//
// The three methods live on the PROTOTYPE and not on the object, which is where
// 27.5.1 puts them: a generator object therefore has no own property of any
// kind, and `Object.getOwnPropertyNames` and `getOwnPropertySymbols` of one are
// both empty.

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/generator.h"
#include "runtime/heap.h"
#include "runtime/iterator.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {
namespace {

Value readSlot(Rooted<Value>& obj, uint32_t slot) {
    return obj.get().asObject<ObjectHeader>()->internalSlot(slot);
}

void writeSlot(Rooted<Value>& obj, uint32_t slot, Value val) {
    obj.get().asObject<ObjectHeader>()->setInternalSlot(slot, val);
}

uint32_t stateOf(Rooted<Value>& gen) {
    return static_cast<uint32_t>(readSlot(gen, GeneratorSlot::State).asNumber());
}

void setState(Rooted<Value>& gen, uint32_t state) {
    writeSlot(gen, GeneratorSlot::State, Value::fromDouble(static_cast<double>(state)));
}

// 7.4.1 CreateIterResultObject, in the field order the spec writes it —
// `value` then `done`, which is the order `Object.keys` of one reports.
Value iterResult(Rooted<Value>& value, bool done) {
    Rooted<Value> out{Value(bronze_create_object())};
    Rooted<Value> vk{rtMakeString("value")};
    out.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), vk, value);
    Rooted<Value> dk{rtMakeString("done")};
    Rooted<Value> dv{Value::fromBool(done)};
    out.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), dk, dv);
    return out.get();
}

// The `done` of the result the resume function built. It is an ordinary object
// this compilation's own code created two lines ago, so the read is a plain
// property get with no protocol around it.
bool resultIsDone(Rooted<Value>& result) {
    if (!result.get().isObject()) return true;
    Rooted<Value> key{rtMakeString("done")};
    return bronze_truthy(
        result.get().asObject<ObjectHeader>()->getProp(rtHeap(), key).rawBits());
}

// 27.5.3.1 GeneratorValidate, plus the resumption itself. One function for all
// three methods because the spec's two — GeneratorResume and
// GeneratorResumeAbrupt — differ in exactly what they do in the states that
// never reach the body, and share every step that does.
uint64_t resume(uint64_t thisBits, uint32_t mode, Rooted<Value>& sent, const char* method) {
    Rooted<Value> self{Value(thisBits)};
    // The brand (27.5.3.1 step 2): a [[GeneratorState]] slot, asked for the way
    // every other iterator kind's is — the kind's prototype AND the kind's slot
    // count, because a forged prototype has no slots to read.
    if (!rtIsIteratorObject(self.get(), IteratorProto::Generator)) {
        return rtThrowTypeError("Generator.prototype." + std::string(method) +
                                " called on a value that is not a generator")
            .rawBits();
    }
    const uint32_t state = stateOf(self);
    if (state == GeneratorState::Executing) {
        // 27.5.3.2 step 2. The generator is on the stack below this call, and
        // re-entering it would run one body in two places at once.
        return rtThrowTypeError("Generator.prototype." + std::string(method) +
                                " called on a generator that is already running")
            .rawBits();
    }
    if (state == GeneratorState::Completed) {
        if (mode == GeneratorResumeMode::Throw) return rtThrow(sent.get()).rawBits();
        // `next()` answers undefined and `return(v)` answers v, both done.
        Rooted<Value> value{mode == GeneratorResumeMode::Return ? sent.get()
                                                                : Value::fromUndefined()};
        return iterResult(value, true).rawBits();
    }
    if (state == GeneratorState::SuspendedStart && mode != GeneratorResumeMode::Next) {
        // 27.5.3.3 on a generator that has not started: the body never runs at
        // all, so there is no `finally` to reach and nothing to resume.
        setState(self, GeneratorState::Completed);
        if (mode == GeneratorResumeMode::Throw) return rtThrow(sent.get()).rawBits();
        return iterResult(sent, true).rawBits();
    }

    Rooted<Value> body{readSlot(self, GeneratorSlot::Resume)};
    setState(self, GeneratorState::Executing);
    Value args[2] = {Value::fromDouble(static_cast<double>(mode)), sent.get()};
    Rooted<Value> result{
        body.get().asObject<FunctionHeader>()->call(Value::fromUndefined(), 2, args)};
    if (rtExceptionPending()) {
        // An exception out of the body ends the walk (27.5.3.2 step 8 leaves
        // the generator completed however the resumption finished).
        setState(self, GeneratorState::Completed);
        return Value::fromUndefined().rawBits();
    }
    setState(self, resultIsDone(result) ? GeneratorState::Completed
                                        : GeneratorState::SuspendedYield);
    return result.get().rawBits();
}

uint64_t generatorNext(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> sent{args[0]};
    return resume(thisBits, GeneratorResumeMode::Next, sent, "next");
}

uint64_t generatorReturn(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> sent{args[0]};
    return resume(thisBits, GeneratorResumeMode::Return, sent, "return");
}

uint64_t generatorThrow(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> sent{args[0]};
    return resume(thisBits, GeneratorResumeMode::Throw, sent, "throw");
}

}  // namespace

void rtInstallGeneratorPrototype(Rooted<Value>& proto) {
    struct Method {
        const char* key;
        NativeFunctionCode code;
        uint32_t arity;
    };
    // 27.5.1.2 through 27.5.1.4, in the order the spec lists them, each taking
    // one argument.
    const Method methods[] = {
        {"next", generatorNext, 1},
        {"return", generatorReturn, 1},
        {"throw", generatorThrow, 1},
    };
    for (const auto& method : methods) {
        Rooted<Value> fn{Value(bronze_function_singleton(method.code, method.arity))};
        Rooted<Value> key{rtMakeString(method.key)};
        proto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, fn);
    }
}

}  // namespace bronze::runtime

extern "C" {

// A GENERATOR OBJECT (ECMA-262 27.5.1): %GeneratorPrototype% for a prototype,
// [[GeneratorState]] at suspendedStart, and [[GeneratorContext]] holding the
// closure that IS the body. The two internal slots are real fields rather than
// properties under a reserved name, which is what keeps a generator object's
// own-key list empty — `next` is inherited, and the state is not a property at
// all.
uint64_t bronze_create_generator_object(uint64_t resumeBits) {
    using namespace bronze;
    using namespace bronze::runtime;
    Rooted<Value> body{Value(resumeBits)};
    Rooted<Value> gen{rtNewIteratorObject(IteratorProto::Generator)};
    // Written after the allocation above, through the roots: `rtNewIteratorObject`
    // can collect, and a by-value copy of `body` taken before it would point
    // into dead from-space.
    gen.get().asObject<ObjectHeader>()->setInternalSlot(
        GeneratorSlot::State, Value::fromDouble(GeneratorState::SuspendedStart));
    gen.get().asObject<ObjectHeader>()->setInternalSlot(GeneratorSlot::Resume, body.get());
    return gen.get().rawBits();
}

}  // extern "C"
