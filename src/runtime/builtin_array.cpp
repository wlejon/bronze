#include <cstdint>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/builtin_array_internal.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

namespace bronze::runtime {

Value rtArraySpeciesCreate(Rooted<Value>& originalArray, uint32_t length) {
    if (!isArray(originalArray.get())) {
        return Value(bronze_create_array(length));
    }
    Rooted<Value> ctorKey{rtMakeString("constructor")};
    PropertyInfo info;
    Value ctorVal = Value::fromUndefined();
    if (ObjectHeader* holder = rtSymbolKeyHolder(originalArray.get())) {
        if (holder->shape &&
            holder->shape->lookupProperty(
                PropertyKey::forString(ctorKey.get().asString<StringHeader>()), info)) {
            ctorVal = holder->getProp(rtHeap(), ctorKey, nullptr, originalArray.slot_ptr());
        }
    }
    if (ctorVal.isUndefined()) {
        ctorVal = rtArrayConstructorObject();
    }
    if (ctorVal.isObject()) {
        Rooted<Value> ctorRoot{ctorVal};
        Rooted<Value> speciesKey{Value::fromSymbol(rtSymbolSpecies())};
        Value speciesVal(bronze_elem_get(ctorRoot.get().rawBits(), speciesKey.get().rawBits()));
        if (speciesVal.isNull()) return Value(bronze_create_array(length));
        if (!speciesVal.isUndefined()) {
            if (!isCallable(speciesVal)) {
                return rtThrowTypeError("Symbol.species is not a constructor");
            }
            uint64_t lenArg = Value::fromDouble(length).rawBits();
            return Value(bronze_construct(speciesVal.rawBits(), 1, &lenArg));
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
    {"push", arrayPush, 0},
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

Value g_arrayPrototype = Value::fromUndefined();

}  // namespace

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
    for (const ArrayMethod& m : kArrayMethods) {
        if (key == m.name) return rtNativeFunction(m.code, m.arity);
    }
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
