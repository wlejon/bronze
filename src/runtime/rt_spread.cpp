// Spread, rest and the destructuring source check (docs/0017).
//
// What these have in common is a LENGTH that is not known where the code was
// generated: `[0, ...a, 4]` has as many elements as `a` had at that moment,
// `f(...args)` passes as many arguments, and `...rest` collects however many
// were left. Compiled code therefore builds a container and hands it here
// rather than emitting an indexed store per element.
//
// The iterable walk is the one docs/0012 decision 2 built for for-of — index
// based, code points for a string — so a spread and a for-of over the same
// value visit exactly the same elements.

#include <cstdio>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
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
    arr->header.flags = 1;
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

bool isArray(Value v) { return v.isObject() && v.asObject<HeapObjectHeader>()->flags == 1; }

bool isWalkable(Value v) {
    if (v.isString()) return true;
    if (!v.isObject()) return false;
    const uint32_t flags = v.asObject<HeapObjectHeader>()->flags;
    return flags == 1 || flags == 3;
}

// Named, so a value bronze cannot walk says which construct asked. The three
// callers differ only in that word, and printing "for-of" for a spread was
// the kind of misdirection a diagnostic exists to avoid.
[[noreturn]] void notWalkable(const char* what) {
    char msg[128];
    std::snprintf(msg, sizeof(msg), "%s of a value that is not an array, string or typed array",
                  what);
    fatal(msg);
}

// Every element of `src` from `from` on, appended to `out`. The length is
// re-read per step for the same reason for-of re-reads it: it is a property
// of the value, not a snapshot.
void appendIterable(Rooted<Value>& out, Rooted<Value>& src, double from, const char* what) {
    if (!isWalkable(src.get())) notWalkable(what);
    for (double i = from;;) {
        const double length = bronze_iter_length(src.get().rawBits());
        if (!(i < length)) break;
        Rooted<Value> elem{Value(bronze_iter_at(src.get().rawBits(), i))};
        appendTo(out, elem);
        i = bronze_iter_advance(src.get().rawBits(), i);
    }
}

// One own enumerable property, copied into `target` under the same key. The
// key is an arena-interned shape string, so it is copied into the heap first:
// the result holds ordinary JS strings, never pointers into the shape arena.
void copyProperty(Rooted<Value>& target, Rooted<Value>& source, StringHeader* name) {
    Rooted<Value> key{rtCopyKeyToHeap(name)};
    Rooted<Value> val{source.get().asObject<ObjectHeader>()->getProp(rtHeap(), key)};
    target.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
}

}  // namespace

extern "C" {

// The source of a destructuring, checked ONCE before any element is read.
//
// Two things make it worth its own instruction. Destructuring `null` or
// `undefined` is a TypeError in ECMA-262 and bronze has no `throw`, so it is
// a hard error here rather than a set of quietly `undefined` bindings. And
// checking up front is what lets every read below it name the CONSTRUCT that
// asked — without it, `const [a] = 5` would report a for-of error.
uint64_t bronze_pattern_check(uint64_t vBits, uint32_t kind) {
    Value v(vBits);
    if (v.isUndefined() || v.isNull()) {
        fatal(kind == kPatternArray
                  ? "array destructuring of null or undefined"
                  : "object destructuring of null or undefined");
    }
    if (kind == kPatternArray && !isWalkable(v)) {
        notWalkable("array destructuring");
    }
    (void)kPatternObject;
    return vBits;
}

// `[p, ...rest] = xs` — everything from the cursor on, as a fresh array.
// Empty rather than `undefined` when nothing is left, which is the whole
// point of a rest element: it is always a container.
uint64_t bronze_iter_rest(uint64_t vBits, double index) {
    Rooted<Value> src{Value(vBits)};
    Rooted<Value> out{newArray()};
    appendIterable(out, src, index, "array destructuring");
    return out.get().rawBits();
}

// `function f(a, ...rest)` — the arguments past the fixed ones, as an array.
// Called from the generated call wrapper, which is the only place that can
// see the caller's real argument count (docs/0017 decision 2).
uint64_t bronze_rest_args(uint32_t argc, const uint64_t* argv, uint32_t first) {
    Rooted<Value> out{newArray()};
    for (uint32_t i = first; i < argc; ++i) {
        Rooted<Value> elem{Value(argv[i])};
        appendTo(out, elem);
    }
    return out.get().rawBits();
}

void bronze_array_append(uint64_t arrBits, uint64_t valBits) {
    Rooted<Value> arr{Value(arrBits)};
    Rooted<Value> val{Value(valBits)};
    appendTo(arr, val);
}

void bronze_array_spread(uint64_t arrBits, uint64_t srcBits) {
    Rooted<Value> arr{Value(arrBits)};
    Rooted<Value> src{Value(srcBits)};
    appendIterable(arr, src, 0.0, "spread");
}

// `{ ...src }` — CopyDataProperties (ECMA-262 7.3.25) over own ENUMERABLE
// string keys, in the order docs/0009 pins, at the position the spread was
// written. Later keys overwrite earlier ones because this is an ordinary
// property write into the object being built.
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
        // enumerable and is deliberately not copied.
        const uint32_t length = srcVal.asObject<ArrayHeader>()->length;
        for (uint32_t i = 0; i < length; ++i) {
            Rooted<Value> key{Value::fromDouble(static_cast<double>(i))};
            Rooted<Value> val{src.get().asObject<ArrayHeader>()->getElem(i)};
            bronze_elem_set(target.get().rawBits(), key.get().rawBits(), val.get().rawBits());
        }
        return;
    }
    if (!isPlainObject(srcVal)) {
        fatal("object spread of a value that is not a plain object, an array, null or "
              "undefined");
    }
    for (StringHeader* name : rtOwnKeysOrdered(src.get().asObject<ObjectHeader>())) {
        copyProperty(target, src, name);
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
    for (StringHeader* name : rtOwnKeysOrdered(src.get().asObject<ObjectHeader>())) {
        bool skip = false;
        if (isArray(excluded.get())) {
            // Re-derived per iteration: copyProperty below allocates, and a
            // collection moves both the key array and its element block.
            auto* keys = excluded.get().asObject<ArrayHeader>();
            for (uint32_t i = 0; i < keys->length && !skip; ++i) {
                Value k = keys->getElem(i);
                skip = k.isString() && k.asString<StringHeader>()->equals(*name);
            }
        }
        if (skip) continue;
        copyProperty(out, src, name);
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
    std::vector<uint64_t> block(args->length);
    for (uint32_t i = 0; i < args->length; ++i) block[i] = args->getElem(i).rawBits();
    return bronze_dynamic_call(calleeBits, thisBits, static_cast<uint32_t>(block.size()),
                               block.data());
}

uint64_t bronze_construct_spread(uint64_t calleeBits, uint64_t argsBits) {
    Value argsVal(argsBits);
    if (!isArray(argsVal)) {
        fatal("internal: spread constructor arguments are not an array");
    }
    auto* args = argsVal.asObject<ArrayHeader>();
    std::vector<uint64_t> block(args->length);
    for (uint32_t i = 0; i < args->length; ++i) block[i] = args->getElem(i).rawBits();
    return bronze_construct(calleeBits, static_cast<uint32_t>(block.size()), block.data());
}

}  // extern "C"

}  // namespace bronze::runtime
