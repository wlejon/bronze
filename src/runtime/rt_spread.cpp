// Spread, rest and the destructuring source check.
//
// What these have in common is a LENGTH that is not known where the code was
// generated: `[0, ...a, 4]` has as many elements as `a` had at that moment,
// `f(...args)` passes as many arguments, and `...rest` collects however many
// were left. Compiled code therefore builds a container and hands it here
// rather than emitting an indexed store per element.
//
// The iterable walk is the one for-of uses — index based, code points for a
// string — so a spread and a for-of over the same value visit exactly the same
// elements.

#include <cstdio>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// The kind a `pattern.check` names, matching the immediate lowering emits.
constexpr uint32_t kPatternArray = 0;
constexpr uint32_t kPatternObject = 1;

Value newArray() {
    ArrayHeader* arr = ArrayHeader::create(rtHeap(), 4);
    arr->header.flags = HeapKind::Array;
    arr->length = 0;
    return Value::fromObject(arr);
}

// Append through the root: growth reallocates the element block and can move
// the array itself, so neither pointer survives the write.
void appendTo(Rooted<Value>& arrRoot, Rooted<Value>& val) {
    const uint32_t at = arrRoot.get().asObject<ArrayHeader>()->length;
    arrRoot.get().asObject<ArrayHeader>()->setElem(rtHeap(), at, val);
}

bool isPlainObject(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == BRONZE_ABI_OBJ_FLAGS_PLAIN;
}

// %ThrowTypeError% (ECMA-262 10.2.4), as far as bronze can honestly go.
//
// A read of `arguments.callee` wants the function object of the RUNNING
// invocation. bronze's calling convention does not pass one: a compiled body is
// entered through its code pointer with `(env, this, arguments, ...params)`,
// and the FunctionHeader that wraps it is not among them — a direct typed call
// does not construct one at all. So 10.4.4's sloppy answer, which IS that
// function, cannot be produced without a new channel on every call in the
// program.
//
// Strict code's answer is a TypeError and bronze could give that exactly — but
// only by knowing which mode the function was written in, and strictness is a
// per-instruction fact in lowering that no IL function carries down to here. So
// one loud answer covers both modes and names the gap, which is the house rule
// applied to the alternative: a silent `undefined`, which is what a program
// feature-detecting `callee` used to read.
[[noreturn]] uint64_t argumentsCalleePill(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    fatal("unsupported: `arguments.callee` — the running function object is not passed to a "
          "compiled body, so sloppy code cannot be handed the function (10.4.4) and strict "
          "code cannot be told apart to be handed the TypeError (10.2.4)");
}

// The arena-interned `callee` key. Interned once because every call that owns
// an `arguments` object defines this property, and a heap string per call would
// be an allocation the property never keeps.
StringHeader* calleeKey() {
    static StringHeader* key = nullptr;
    if (!key) {
        Rooted<Value> name{rtMakeString("callee")};
        key = StringHeader::internToArena(rtArena(), name.get().asString<StringHeader>());
    }
    return key;
}

// 10.2.11 step 6, on the array that stands in for the arguments object. The
// pair is non-enumerable, which is both what the specification says and what
// keeps every existing walk over an arguments object unchanged: `for-in`,
// `Object.keys`, spread and `JSON.stringify` all ask for own enumerable keys,
// and console.log's array format skips accessors outright.
void installArgumentsCallee(Rooted<Value>& args) {
    ArrayHeader::ensureProperties(rtHeap(), rtArena(), args);
    Rooted<Value> props{args.get().asObject<ArrayHeader>()->properties};
    Rooted<Value> key{Value::fromString(calleeKey())};
    Rooted<Value> pill{Value(bronze_function_singleton(
        reinterpret_cast<bronze_fn_code>(&argumentsCalleePill), 0))};
    ObjectHeader::defineAccessor(rtHeap(), rtArena(), props, key, pill, pill,
                                 /*enumerable=*/false);
}

bool isArray(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Array;
}

// Every element of `src`, appended to `out`, through the iterator protocol — so
// `[...someSet]` and `f(...someMap)` walk the same way `for-of` does, which is
// the whole reason spread stopped being an index walk. A value with no
// @@iterator method raises the TypeError 7.4.2 defines rather than producing an
// empty result.
void appendIterable(Rooted<Value>& out, Rooted<Value>& src) {
    Rooted<Value> rec{Value(bronze_iter_open(src.get().rawBits()))};
    if (rtExceptionPending()) return;
    while (bronze_iter_step(rec.get().rawBits())) {
        Rooted<Value> elem{Value(bronze_iter_value(rec.get().rawBits()))};
        appendTo(out, elem);
    }
}

// One own enumerable property, copied into `target` under the same key. The
// key is an arena-interned shape string, so it is copied into the heap first:
// the result holds ordinary JS strings, never pointers into the shape arena.
void copyProperty(Rooted<Value>& target, Rooted<Value>& source, PropertyKey name) {
    Rooted<Value> key{name.isSymbol() ? name.toValue() : rtCopyKeyToHeap(name.string())};
    Rooted<Value> val{source.get().asObject<ObjectHeader>()->getProp(rtHeap(), key)};
    target.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
}

}  // namespace

extern "C" {

// The source of a destructuring, checked ONCE before any element is read.
//
// Two things make it worth its own instruction. Destructuring `null` or
// `undefined` is a TypeError (7.3.20 RequireObjectCoercible, reached by
// 8.6.2 BindingInitialization), so it is raised here rather than left as a
// set of quietly `undefined` bindings. And checking up front is what lets
// every read below it name the CONSTRUCT that asked — without it,
// `const [a] = 5` would report a for-of error.
//
// The value is returned unchanged on the raising path too. Generated code
// stores this result into a GC root slot before it tests the pending cell, and
// the slot has to hold something the collector can parse.
uint64_t bronze_pattern_check(uint64_t vBits, uint32_t kind) {
    Value v(vBits);
    if (v.isUndefined() || v.isNull()) {
        rtThrowTypeError(kind == kPatternArray
                             ? "array destructuring of null or undefined"
                             : "object destructuring of null or undefined");
        return vBits;
    }
    // An array pattern's source no longer needs a kind test here: `iter.open`
    // is the next instruction and 7.4.2 already raises for a value with no
    // @@iterator method. What survives is the null/undefined case, which
    // 8.6.2 reaches through RequireObjectCoercible BEFORE GetIterator and
    // which therefore has to name the CONSTRUCT rather than the protocol.
    (void)kPatternArray;
    (void)kPatternObject;
    return vBits;
}

// `function f(a,...rest)` — the arguments past the fixed ones, as an array.
// Called from the generated call wrapper, which is the only place that can see
// the caller's real argument count.
uint64_t bronze_rest_args(uint32_t argc, const uint64_t* argv, uint32_t first) {
    // The copy comes FIRST, before `newArray` — this is RootedArgs' contract
    // ("read arguments from HERE and never from `argv` again"), and allocating
    // ahead of it broke exactly the case that contract is written for. `argv`
    // is only self-protecting when the caller is GENERATED code, whose block
    // lives in its GC root frame; the blocks builtins build for callbacks
    // (`builtin_array.cpp`'s `Value block[3]`, the JSON replacer's, the regexp
    // replacer's) are plain stack memory nothing scans. So
    // `items.map(function (...a) { ... })` collected inside `newArray`, moved
    // the elements the caller had already written into that block, and every
    // later iteration read a forwarded header — which still reports its old
    // tag, so it answered wrongly and silently rather than crashing.
    RootedArgs args(argc, argv);
    Rooted<Value> out{newArray()};
    for (uint32_t i = first; i < argc; ++i) {
        Rooted<Value> elem{args[i]};
        appendTo(out, elem);
    }
    return out.get().rawBits();
}

// The `arguments` object of one call: every argument the caller actually
// passed, in order. Built by the call wrapper for the same reason the rest
// array is — it is the only code that can see the real argument count.
//
// UNMAPPED, and an ordinary array: writing `arguments[0]` does not write the
// parameter, and `Array.isArray(arguments)` answers true where a spec engine
// answers false. Both are deliberate divergences.
//
// `callee` is the one own property the array is given, and it is what tells an
// arguments object apart from every other array — which is why the answer is a
// PROPERTY here rather than a name recognised on the array read path: `[].callee`
// is `undefined` and must stay `undefined`, and nothing else could distinguish
// the two receivers. 10.2.11 step 6 defines it on the unmapped object as an
// accessor pair whose halves are %ThrowTypeError%, so the shape of what is
// installed is the specification's; only the pill's contents differ.
uint64_t bronze_arguments_object(uint32_t argc, const uint64_t* argv) {
    Rooted<Value> args{Value(bronze_rest_args(argc, argv, 0))};
    installArgumentsCallee(args);
    return args.get().rawBits();
}

// One argument, or `undefined` when the caller passed fewer. Every other
// function's wrapper reads argv unguarded, because `FunctionHeader::call` has
// already padded it up to the declared arity — and a function that owns an
// `arguments` object declares arity 0 precisely so that padding does NOT
// happen, since `f(1)` and `f(1, undefined)` must give `arguments.length` 1 and
// 2. Guarding the read is what that costs.
uint64_t bronze_arg_at(uint32_t argc, const uint64_t* argv, uint32_t index) {
    if (index >= argc) return BRONZE_ABI_UNDEFINED_BITS;
    return argv[index];
}

void bronze_array_append(uint64_t arrBits, uint64_t valBits) {
    Rooted<Value> arr{Value(arrBits)};
    Rooted<Value> val{Value(valBits)};
    appendTo(arr, val);
}

void bronze_array_spread(uint64_t arrBits, uint64_t srcBits) {
    Rooted<Value> arr{Value(arrBits)};
    Rooted<Value> src{Value(srcBits)};
    appendIterable(arr, src);
}

// `{...src }` — CopyDataProperties (ECMA-262 7.3.25) over own ENUMERABLE string
// keys, in own enumerable order, at the position the spread was written. Later
// keys overwrite earlier ones because this is an ordinary property write into
// the object being built.
//
// `null` and `undefined` contribute nothing, which the spec says and which
// `{ ...maybeOptions }` relies on. Every other non-object source is a named
// hard error rather than the spec's silent empty result: bronze has no
// wrapper objects to read index properties off a primitive, and a quiet `{}`
// is the shape of bug that hides longest.
void bronze_object_spread(uint64_t objBits, uint64_t srcBits) {
    Value srcVal(srcBits);
    if (srcVal.isUndefined() || srcVal.isNull()) return;

    Rooted<Value> target{Value(objBits)};
    Rooted<Value> src{srcVal};

    if (isArray(srcVal)) {
        // An array's own enumerable keys are its indices; `length` is not
        // enumerable and is deliberately not copied. A HOLE is not an own key
        // either, so `{...a }` after `delete a[1]` has no `'1'` at all — the
        // same set `Object.keys`, `for-in` and `in` report, which is the whole
        // point of asking one question in one way.
        const uint32_t length = srcVal.asObject<ArrayHeader>()->length;
        for (uint32_t i = 0; i < length; ++i) {
            if (!src.get().asObject<ArrayHeader>()->hasElem(i)) continue;
            Rooted<Value> key{Value::fromDouble(static_cast<double>(i))};
            Rooted<Value> val{src.get().asObject<ArrayHeader>()->getElem(i)};
            bronze_elem_set(target.get().rawBits(), key.get().rawBits(), val.get().rawBits(), /*strict=*/false);
        }
        return;
    }
    if (!isPlainObject(srcVal)) {
        fatal("object spread of a value that is not a plain object, an array, null or "
              "undefined");
    }
    // Own enumerable keys of BOTH kinds: 7.3.25 CopyDataProperties takes
    // OwnPropertyKeys, not the string half of it, so `{ ...o }` and
    // `Object.assign` carry a symbol-keyed property across. That is the one
    // enumeration in the language that does.
    for (PropertyKey name : rtOwnKeysOrdered(src.get().asObject<ObjectHeader>())) {
        copyProperty(target, src, name);
        // `copyProperty` reads with Get, so a source property that is an
        // accessor runs user code. Copying the next one after that threw would
        // be the runtime continuing past an exception.
        if (rtExceptionPending()) return;
    }
}

// `const { a, ...others } = src` — a fresh object holding every own
// enumerable property of `src` except the ones the pattern already named.
// The excluded keys arrive as an ARRAY rather than as a compile-time list
// because a computed key is not known until the pattern runs.
uint64_t bronze_object_rest(uint64_t srcBits, uint64_t excludedBits) {
    Value srcVal(srcBits);
    Rooted<Value> out{Value(bronze_create_object())};
    if (srcVal.isUndefined() || srcVal.isNull()) return out.get().rawBits();
    if (!isPlainObject(srcVal)) {
        fatal("object rest from a value that is not a plain object");
    }

    // The exclusions are compared as STRINGS, after the same ToPropertyKey
    // the pattern's own reads went through, so `{ [1]: v, ...rest }` excludes
    // the property `"1"` the read actually took.
    Rooted<Value> excluded{Value(excludedBits)};
    Rooted<Value> src{srcVal};
    for (PropertyKey name : rtOwnKeysOrdered(src.get().asObject<ObjectHeader>())) {
        bool skip = false;
        if (isArray(excluded.get())) {
            // Re-derived per iteration: copyProperty below allocates, and a
            // collection moves both the key array and its element block.
            auto* keys = excluded.get().asObject<ArrayHeader>();
            for (uint32_t i = 0; i < keys->length && !skip; ++i) {
                Value k = keys->getElem(i);
                skip = PropertyKey::fromValue(k).matches(name);
            }
        }
        if (skip) continue;
        copyProperty(out, src, name);
        if (rtExceptionPending()) return out.get().rawBits();
    }
    return out.get().rawBits();
}

// A call whose argument list held a spread: the arguments were built into an
// array because their count is a runtime fact, and this unpacks it into the
// uniform convention. The copy is the RootedArgs contract in reverse — the
// vector is plain memory, and it is safe for the same reason
// `FunctionHeader::call`'s arity-adaptation vector is: the callee's prologue
// roots its parameters before it can allocate.
uint64_t bronze_dynamic_call_spread(uint64_t calleeBits, uint64_t thisBits, uint64_t argsBits) {
    Value argsVal(argsBits);
    if (!isArray(argsVal)) {
        fatal("internal: spread call arguments are not an array");
    }
    auto* args = argsVal.asObject<ArrayHeader>();
    RootedBlock block(args->length);
    for (uint32_t i = 0; i < args->length; ++i) block.set(i, args->getElem(i));
    return bronze_dynamic_call(calleeBits, thisBits, block.count(), block.data());
}

uint64_t bronze_construct_spread(uint64_t calleeBits, uint64_t argsBits) {
    Value argsVal(argsBits);
    if (!isArray(argsVal)) {
        fatal("internal: spread constructor arguments are not an array");
    }
    auto* args = argsVal.asObject<ArrayHeader>();
    // Rooted, unlike every other block the runtime builds, because
    // `bronze_construct` allocates the instance BEFORE it reads this — see
    // RootedBlock. A plain vector here segfaulted under `BRONZE_GC_STRESS=1` on
    // `new Pair(...[{n:1},{n:2}])`.
    RootedBlock block(args->length);
    for (uint32_t i = 0; i < args->length; ++i) block.set(i, args->getElem(i));
    return bronze_construct(calleeBits, block.count(), block.data());
}

}  // extern "C"

}  // namespace bronze::runtime
