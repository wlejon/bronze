#include <cstdint>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/builtin_array_internal.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/native_base.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

namespace bronze::runtime {

// 7.3.22 ArraySpeciesCreate. `map`, `filter`, `slice`, `splice`, `concat`,
// `flat`, `flatMap` and `toSpliced` all build their result through it, so a
// subclass's `map` answers a subclass instance and `static get
// [Symbol.species]() { return Array; }` opts back out.
//
// THE FAST PATH IS THE FIRST TWO LINES, and it is not an optimization detail:
// steps 3-6 are two property reads and a construct, and the overwhelming
// majority of arrays in any program are plain ones whose `constructor` is
// %Array% and whose @@species is %Array% — for which the whole algorithm is
// `ArrayCreate(length)`. An array carries a property BOX only when a program
// subclassed it or wrote a named property on it (runtime/native_base.h), so
// "no box" IS "the constructor is exactly the Array intrinsic", answered by one
// load with no property path entered at all.
Value rtArraySpeciesCreate(Rooted<Value>& originalArray, uint32_t length) {
    if (!isArray(originalArray.get())) {
        return Value(bronze_create_array(length));
    }
    if (!rtExoticPropertyBox(originalArray.get()).isObject()) {
        return Value(bronze_create_array(length));
    }
    // Step 5: Get(originalArray, "constructor"), which for a subclass instance
    // is `A.prototype.constructor` up the chain the box carries — and for an
    // array carrying only an expando is absent, leaving %Array%.
    Rooted<Value> ctorKey{rtMakeString("constructor")};
    // Both reads below are held in ROOTS from the moment they exist, because
    // every predicate applied to them — `rtIsArrayConstructor` above all — can
    // build the intrinsic it compares against, and an identity test run against
    // a pre-collection address answers a confident no.
    Rooted<Value> ctorRoot{Value::fromUndefined()};
    if (Value found;
        rtExoticNamedRead(originalArray, ctorKey.get().asString<StringHeader>(), found)) {
        ctorRoot.set(found);
    }
    // `Array` itself takes the same shortcut the missing-box case does: its
    // @@species is itself (rt_prop.cpp), so constructing through it would run
    // 23.1.1.1 to reach exactly ArrayCreate(length).
    if (ctorRoot.get().isUndefined() || rtIsArrayConstructor(ctorRoot.get())) {
        return Value(bronze_create_array(length));
    }
    if (ctorRoot.get().isObject()) {
        Rooted<Value> speciesKey{Value::fromSymbol(rtSymbolSpecies())};
        Rooted<Value> speciesRoot{
            Value(bronze_elem_get(ctorRoot.get().rawBits(), speciesKey.get().rawBits()))};
        if (rtExceptionPending()) return Value(bronze_create_array(0));
        // Step 6: @@species NULL means "no subclass result", which is the
        // documented opt-out and is NOT the same as absent.
        if (speciesRoot.get().isNull()) return Value(bronze_create_array(length));
        if (!speciesRoot.get().isUndefined()) {
            if (rtIsArrayConstructor(speciesRoot.get())) {
                return Value(bronze_create_array(length));
            }
            if (!isCallable(speciesRoot.get())) {
                return rtThrowTypeError("Symbol.species is not a constructor");
            }
            Rooted<Value> lenRoot{Value::fromDouble(length)};
            return Value(bronze_construct(speciesRoot.get().rawBits(), 1,
                                          reinterpret_cast<const uint64_t*>(lenRoot.slot_ptr())));
        }
    }
    return Value(bronze_create_array(length));
}

bool rtIsConcatSpreadable(Rooted<Value>& item) {
    if (!item.get().isObject()) return false;
    Rooted<Value> spreadKey{Value::fromSymbol(rtSymbolIsConcatSpreadable())};
    Value spreadVal(bronze_elem_get(item.get().rawBits(), spreadKey.get().rawBits()));
    if (!spreadVal.isUndefined()) {
        return bronze_truthy(spreadVal.rawBits());
    }
    return isArray(item.get());
}

namespace {

struct ArrayMethod {
    const char* name;
    bronze_fn_code code;
    uint32_t arity;
};

const ArrayMethod kArrayMethods[] = {
    {"at", arrayAt, 1},
    {"concat", arrayConcat, 0},
    {"copyWithin", arrayCopyWithin, 0},
    {"entries", rtArrayEntriesBuiltin, 0},
    {"every", arrayEvery, 1},
    {"fill", arrayFill, 0},
    {"filter", arrayFilter, 1},
    {"find", arrayFind, 1},
    {"findIndex", arrayFindIndex, 1},
    {"findLast", arrayFindLast, 1},
    {"findLastIndex", arrayFindLastIndex, 1},
    {"flat", arrayFlat, 0},
    {"flatMap", arrayFlatMap, 1},
    {"forEach", arrayForEach, 1},
    {"includes", arrayIncludes, 1},
    {"indexOf", arrayIndexOf, 1},
    {"join", arrayJoin, 0},
    {"keys", rtArrayKeysBuiltin, 0},
    {"lastIndexOf", arrayLastIndexOf, 1},
    {"map", arrayMap, 1},
    {"pop", arrayPop, 0},
    {"push", bronze_array_push, 0},
    {"reduce", arrayReduce, 0},
    {"reduceRight", arrayReduceRight, 0},
    {"reverse", arrayReverse, 0},
    {"shift", arrayShift, 0},
    {"slice", arraySlice, 0},
    {"some", arraySome, 1},
    {"sort", rtArraySortBuiltin, 1},
    {"splice", arraySplice, 0},
    {"toReversed", arrayToReversed, 0},
    {"toSorted", arrayToSorted, 1},
    {"toSpliced", arrayToSpliced, 0},
    {"toString", rtArrayToStringBuiltin, 0},
    {"unshift", arrayUnshift, 0},
    {"values", rtArrayValuesBuiltin, 0},
    {"with", arrayWith, 2},
};

thread_local Value g_arrayPrototype = Value::fromUndefined();

// Sized once and never resized, so the backing store never moves and the
// pointer published into the TLS block's `array_method_tbl` stays valid —
// generated code reads it through the block on every sentinel IC hit.
static thread_local std::vector<Value> g_arrayMethodValues(1 + std::size(kArrayMethods), Value::fromUndefined());

}  // namespace

void rtVisitArrayMethodRoots(const Heap::RootVisitor& visit) {
    for (Value& v : g_arrayMethodValues) visit(v);
}

uint32_t rtArrayMethodId(const std::string& key) {
    if (key == "constructor") return 0;
    for (size_t i = 0; i < std::size(kArrayMethods); ++i) {
        if (key == kArrayMethods[i].name) return static_cast<uint32_t>(1 + i);
    }
    return UINT32_MAX;
}

Value rtArrayMethodById(uint32_t id) {
    // Published lazily rather than at static init: the sentinel IC only arms
    // after a helper call resolved the method — which goes through here — so
    // by the time generated code reads the block field, the thread running
    // that code has published its own table.
    bronze_tls_block_addr()->array_method_tbl = reinterpret_cast<uint64_t*>(g_arrayMethodValues.data());
    if (id >= g_arrayMethodValues.size()) return Value::fromUndefined();
    if (g_arrayMethodValues[id].isUndefined()) {
        if (id == 0) {
            g_arrayMethodValues[0] = rtArrayConstructorObject();
        } else {
            const size_t idx = id - 1;
            g_arrayMethodValues[id] = rtNativeFunction(kArrayMethods[idx].code, kArrayMethods[idx].arity);
        }
    }
    return g_arrayMethodValues[id];
}

Value rtArrayPrototypeObject() {
    if (g_arrayPrototype.isObject()) return g_arrayPrototype;
    Rooted<Value> objectProto{rtObjectPrototype()};
    Rooted<Value> obj{Value::fromObject(
        ObjectHeader::create(rtHeap(), rtArena(), rtNewRootShape(objectProto.get())))};
    g_arrayPrototype = obj.get();
    rtHeap().add_permanent_root(&g_arrayPrototype);

    for (const ArrayMethod& m : kArrayMethods) {
        Rooted<Value> key{rtMakeString(m.name)};
        Rooted<Value> fn{rtNativeFunction(m.code, m.arity)};
        obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, fn, /*ic=*/nullptr,
                                                    /*enumerable=*/false, /*defineOwn=*/true);
    }
    {
        Rooted<Value> key{rtMakeString("constructor")};
        Rooted<Value> ctor{rtArrayConstructorObject()};
        obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, ctor,
                                                    /*ic=*/nullptr, /*enumerable=*/false,
                                                    /*defineOwn=*/true);
    }
    {
        // 23.1.3's opening sentence: `Array.prototype` IS an Array exotic
        // object, and the one thing about that a program can read off it is
        // its own `length` — 0, writable, neither enumerable nor configurable
        // (10.4.2.1). Without it `Object.create(Array.prototype).length` is
        // `undefined` where the language says 0, which is the divergence a
        // program inheriting from it actually trips over.
        //
        // The object's KIND is still Plain, and that is a deliberate stop
        // rather than an oversight: bronze's prototype links live on shapes and
        // every walk over one — `protoAncestor`, the inline caches, the
        // subclass chain in runtime/native_base.h — dereferences an
        // ObjectHeader. An ArrayHeader carries no shape, so making this object
        // one would leave every `Object.create(Array.prototype)` and every
        // `class extends Array` prototype pointing at something no walk can
        // cross. `Array.isArray` answers for it by identity below instead, and
        // what remains unavailable is element storage ON the prototype, which
        // `Array.prototype[0] = 1` would need.
        Rooted<Value> key{rtMakeString("length")};
        Rooted<Value> zero{Value::fromDouble(0)};
        obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, zero, /*ic=*/nullptr,
                                                    /*enumerable=*/false, /*defineOwn=*/true,
                                                    /*receiver=*/nullptr, /*refused=*/nullptr,
                                                    /*writable=*/true, /*configurable=*/false);
    }
    {
        Rooted<Value> key{rtIteratorKey()};
        Rooted<Value> fn{rtNativeFunction(rtArrayValuesBuiltin, 0)};
        obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, fn, /*ic=*/nullptr,
                                                    /*enumerable=*/false, /*defineOwn=*/true);
    }
    g_arrayPrototype = obj.get();
    return g_arrayPrototype;
}

bool rtIsArrayPrototypeObject(Value v) {
    return g_arrayPrototype.isObject() && v.rawBits() == g_arrayPrototype.rawBits();
}

void rtArrayPrototypeCheckMissingMember(Value obj, const std::string& key) {
    if (rtIsArrayPrototypeObject(obj)) rtCheckArrayMember(key);
}

Value rtArrayMethod(const std::string& key) {
    uint32_t id = rtArrayMethodId(key);
    if (id != UINT32_MAX && id != 0) return rtArrayMethodById(id);
    return Value::fromUndefined();
}

bool rtArrayHasMember(const std::string& key) {
    if (key == "constructor") return true;
    for (const ArrayMethod& m : kArrayMethods) {
        if (key == m.name) return true;
    }
    return rtArrayMemberUnimplemented(key);
}

}  // namespace bronze::runtime
