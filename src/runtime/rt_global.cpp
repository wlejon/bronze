#include <limits>
#include <string>
#include <utility>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/host_globals.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

static thread_local Value g_globalThisObject = Value::fromUndefined();

static const char* const kGlobalObjectNames[] = {
    "Math", "Object", "Number", "JSON", "Array", "String", "Boolean", "Symbol", "BigInt", "RegExp",
    "Promise", "Map", "Set", "WeakMap", "WeakSet", "Error", "TypeError", "AggregateError",
    "RangeError", "SyntaxError", "ReferenceError", "URIError", "isNaN", "isFinite", "parseInt",
    "parseFloat", "ArrayBuffer", "Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array",
    "Uint16Array", "Int32Array", "Uint32Array", "Float32Array", "Float64Array", "DataView",
    "Float16Array", "BigInt64Array", "BigUint64Array", "SharedArrayBuffer", "Atomics",
    "Function", "Proxy", "Reflect", "Date", "encodeURI", "encodeURIComponent", "decodeURI",
    "decodeURIComponent", "escape", "unescape", "Iterator", "WeakRef", "FinalizationRegistry",
    "eval",
};

Value rtGlobalThisObject() {
    if (g_globalThisObject.isUndefined()) {
        Rooted<Value> glob{Value::fromObject(
            ObjectHeader::create(rtHeap(), rtArena(), rtRootShapeForPrototype(Value::fromNull())))};
        glob.get().asObject<HeapObjectHeader>()->flags = BRONZE_ABI_OBJ_FLAGS_PLAIN;
        for (const char* name : kGlobalObjectNames) {
            Value resolved = Value::fromUndefined();
            if (!rtResolveBuiltinGlobal(name, resolved)) {
                fatal((std::string("internal: global object population lists '") + name +
                      "', a name the builtin ladder cannot resolve")
                          .c_str());
            }
            Rooted<Value> val{resolved};
            Rooted<Value> key{rtMakeString(name)};
            glob.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val, nullptr,
                                                    /*enumerable=*/false,
                                                    /*defineOwn='*/true);
        }
        for (const auto& entry : rtHostGlobalEntries()) {
            Rooted<Value> key{rtMakeString(entry.first)};
            Rooted<Value> val{entry.second};
            glob.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val, nullptr,
                                                    /*enumerable=*/false,
                                                    /*defineOwn=*/true);
        }
        {
            const std::pair<const char*, Value> values[] = {
                {"Infinity", Value::fromDouble(std::numeric_limits<double>::infinity())},
                {"NaN", Value::fromDouble(std::numeric_limits<double>::quiet_NaN())},
                {"undefined", Value::fromUndefined()},
            };
            for (const auto& entry : values) {
                Rooted<Value> val{entry.second};
                Rooted<Value> key{rtMakeString(entry.first)};
                glob.get().asObject<ObjectHeader>()->setProp(
                    rtHeap(), rtArena(), key, val, nullptr, /*enumerable=*/false,
                    /*defineOwn=*/true, /*receiver=*/nullptr, /*refused=*/nullptr,
                    /*writable=*/false, /*configurable=*/false);
            }
        }
        Rooted<Value> key{rtMakeString("globalThis")};
        glob.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, glob, nullptr,
                                                /*enumerable=*/false, /*defineOwn=*/true);
        g_globalThisObject = glob.get();
        rtHeap().add_permanent_root(&g_globalThisObject);
    }
    return g_globalThisObject;
}

bool rtGlobalThisOwnLookup(const std::string& name, Value& out) {
    if (!g_globalThisObject.isObject()) return false;
    Rooted<Value> key{rtMakeString(name)};
    PropertyKey pkey = rtInternPropertyKey(key.get());
    auto* obj = g_globalThisObject.asObject<ObjectHeader>();
    uint32_t slot = 0;
    if (!obj->shape || !obj->shape->lookupProperty(pkey, slot)) return false;
    out = g_globalThisObject.asObject<ObjectHeader>()->getProp(rtHeap(), key);
    return true;
}

}  // namespace bronze::runtime
