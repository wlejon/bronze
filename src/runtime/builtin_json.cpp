// The `JSON` namespace — ECMA-262 25.5, and the JavaScript half of it.
//
// The grammar half is `src/json`, a module with no idea bronze's value model
// exists: it turns code units into a tree and rejects everything JavaScript
// allows and JSON does not. That split is a module-isolation decision and not
// an afterthought: JSON's grammar is not JavaScript's, so it cannot borrow
// `src/parse`, and a parser that can be
// driven without a heap is a parser whose rejections can be pinned directly in
// `tests/json`.
//
// What is left here is the conversion — tree to JavaScript values, in a shape
// the collector can survive — plus the reviver, plus the namespace object.

#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "json/json.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

bool isCallable(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function;
}

Value stringFromUnits16(const json::Units& units) {
    std::vector<uint16_t> copy(units.begin(), units.end());
    return rtStringFromUnits(copy);
}

// The parsed tree becomes values bottom-up. The tree is ordinary C++ memory
// and holds no heap pointers, so the walk can allocate freely; only the values
// it has already built need rooting, and each level roots exactly the one it
// is filling.
Value buildValue(const json::Value& node) {
    switch (node.kind) {
        case json::Value::Kind::Null: return Value::fromNull();
        case json::Value::Kind::Bool: return Value::fromBool(node.boolean);
        case json::Value::Kind::Number: return Value::fromDouble(node.number);
        case json::Value::Kind::String: return stringFromUnits16(node.text);
        case json::Value::Kind::Array: {
            Rooted<Value> arr{Value(bronze_create_array(
                static_cast<uint32_t>(node.elements.size())))};
            for (size_t i = 0; i < node.elements.size(); ++i) {
                Rooted<Value> element{buildValue(*node.elements[i])};
                arr.get().asObject<ArrayHeader>()->setElem(rtHeap(), static_cast<uint32_t>(i),
                                                          element);
            }
            return arr.get();
        }
        case json::Value::Kind::Object: {
            Rooted<Value> obj{Value(bronze_create_object())};
            for (const json::Member& member : node.members) {
                Rooted<Value> key{stringFromUnits16(member.key)};
                Rooted<Value> value{buildValue(*member.value)};
                // A repeated key overwrites in place, so the property keeps
                // the position its FIRST appearance gave it — which is what
                // 25.5.1's CreateDataProperty over an ordinary object does,
                // and is observable through Object.keys.
                bronze_elem_set(obj.get().rawBits(), key.get().rawBits(), value.get().rawBits(), /*strict=*/false);
            }
            return obj.get();
        }
    }
    return Value::fromUndefined();
}

// 25.5.1.1 InternalizeJSONProperty: the reviver is called BOTTOM-UP, so a
// nested object has already been rewritten by the time its parent sees it,
// and a reviver that returns `undefined` DELETES the property rather than
// setting it to undefined.
Value internalize(Rooted<Value>& holder, Rooted<Value>& key, Rooted<Value>& reviver);

void internalizeChildren(Rooted<Value>& value, Rooted<Value>& reviver) {
    const bool isArray =
        value.get().isObject() &&
        value.get().asObject<HeapObjectHeader>()->flags == HeapKind::Array;
    if (isArray) {
        const uint32_t length = value.get().asObject<ArrayHeader>()->length;
        for (uint32_t i = 0; i < length; ++i) {
            Rooted<Value> key{rtMakeString(std::to_string(i))};
            Rooted<Value> replaced{internalize(value, key, reviver)};
            if (rtExceptionPending()) return;
            if (replaced.get().isUndefined()) {
                bronze_elem_delete(value.get().rawBits(), key.get().rawBits(), /*strict=*/false);
            } else {
                bronze_elem_set(value.get().rawBits(), key.get().rawBits(),
                                replaced.get().rawBits(), /*strict=*/false);
            }
        }
        return;
    }
    Rooted<Value> keyArray{Value(bronze_object_keys(value.get().rawBits()))};
    if (rtExceptionPending()) return;
    const uint32_t count = keyArray.get().asObject<ArrayHeader>()->length;
    for (uint32_t i = 0; i < count; ++i) {
        Rooted<Value> key{keyArray.get().asObject<ArrayHeader>()->getElem(i)};
        Rooted<Value> replaced{internalize(value, key, reviver)};
        if (rtExceptionPending()) return;
        if (replaced.get().isUndefined()) {
            bronze_elem_delete(value.get().rawBits(), key.get().rawBits(), /*strict=*/false);
        } else {
            bronze_elem_set(value.get().rawBits(), key.get().rawBits(), replaced.get().rawBits(), /*strict=*/false);
        }
    }
}

Value internalize(Rooted<Value>& holder, Rooted<Value>& key, Rooted<Value>& reviver) {
    Rooted<Value> value{Value(bronze_elem_get(holder.get().rawBits(), key.get().rawBits()))};
    if (rtExceptionPending()) return Value::fromUndefined();
    if (value.get().isObject()) {
        const uint16_t flags = value.get().asObject<HeapObjectHeader>()->flags;
        if (flags == HeapKind::Array || flags == BRONZE_ABI_OBJ_FLAGS_PLAIN) {
            internalizeChildren(value, reviver);
            if (rtExceptionPending()) return Value::fromUndefined();
        }
    }
    uint64_t args[2] = {key.get().rawBits(), value.get().rawBits()};
    return Value(bronze_dynamic_call(reviver.get().rawBits(), holder.get().rawBits(), 2, args));
}

uint64_t jsonParse(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> text{rtValueToString(args[0])};
    const std::vector<uint16_t> units = rtStringUnits(text.get().asString<StringHeader>());
    json::Units source(units.begin(), units.end());

    std::string error;
    json::ValuePtr tree = json::parse(source, error);
    if (!tree) {
        // A malformed JSON text is a SyntaxError the language defines, so it is
        // a catchable throw and not a process death. bronze has no SyntaxError
        // constructor of its own yet, so it is raised as an Error carrying the
        // parser's message.
        return rtThrowError(ErrorKind::Error, "SyntaxError: " + error).rawBits();
    }

    Rooted<Value> result{buildValue(*tree)};
    Rooted<Value> reviver{args[1]};
    if (!isCallable(reviver.get())) return result.get().rawBits();

    // 25.5.1 step 7: the reviver walk starts from a wrapper holding the whole
    // document under the empty key, so the root itself can be replaced.
    Rooted<Value> wrapper{Value(bronze_create_object())};
    Rooted<Value> emptyKey{rtMakeString("")};
    bronze_elem_set(wrapper.get().rawBits(), emptyKey.get().rawBits(), result.get().rawBits(), /*strict=*/false);
    return internalize(wrapper, emptyKey, reviver).rawBits();
}

uint64_t jsonStringify(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    return rtJsonStringify(args[0], args[1], args[2]).rawBits();
}

struct NamespaceFn {
    const char* name;
    bronze_fn_code code;
    uint32_t arity;
};

const NamespaceFn kJsonFunctions[] = {
    {"parse", jsonParse, 2},
    {"stringify", jsonStringify, 3},
};

// `JSON` has exactly two function members in ECMA-262, so the only real name
// left is the `Symbol.toStringTag` bronze has no symbols for. It is listed so
// that reading it says so.
const char* const kJsonUnimplemented[] = {
    "rawJSON",
    "isRawJSON",
};

Value g_jsonNamespace = Value::fromUndefined();

}  // namespace

Value rtJsonNamespace() {
    if (g_jsonNamespace.isObject()) return g_jsonNamespace;
    Rooted<Value> obj{Value::fromObject(
        ObjectHeader::create(rtHeap(), rtArena(), rtNewRootShape(Value::fromUndefined())))};
    obj.get().asObject<ObjectHeader>()->header.flags = HeapKind::Plain;
    for (const NamespaceFn& fn : kJsonFunctions) {
        Rooted<Value> key{rtMakeString(fn.name)};
        Rooted<Value> val{Value(bronze_function_singleton(fn.code, fn.arity))};
        obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
    }
    g_jsonNamespace = obj.get();
    rtHeap().add_permanent_root(&g_jsonNamespace);
    return g_jsonNamespace;
}

void rtJsonCheckMissingMember(Value obj, const std::string& key) {
    if (!g_jsonNamespace.isObject() || obj.rawBits() != g_jsonNamespace.rawBits()) return;
    rtCheckUnimplementedMember("JSON", kJsonUnimplemented, std::size(kJsonUnimplemented), key);
}

}  // namespace bronze::runtime
