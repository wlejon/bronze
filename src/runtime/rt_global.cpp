#include <cstring>
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
    "eval", "performance",
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

extern "C" uint64_t bronze_global_get_name(const char* name) {
    if (!name || std::strcmp(name, "globalThis") == 0) {
        return rtGlobalThisObject().rawBits();
    }
    Value host = Value::fromUndefined();
    if (rtHostGlobalLookup(name, host)) {
        return host.rawBits();
    }
    Value resolved = Value::fromUndefined();
    if (rtResolveBuiltinGlobal(name, resolved)) {
        return resolved.rawBits();
    }
    if (rtGlobalThisOwnLookup(name, resolved)) {
        return resolved.rawBits();
    }
    return BRONZE_ABI_UNDEFINED_BITS;
}

extern "C" uint64_t bronze_create_function(bronze_fn_code code, uint32_t arity, uint32_t length,
                                           uint32_t nameKey, uint32_t fnFlags, uint64_t envBits);
extern "C" uint64_t bronze_dynamic_call(uint64_t calleeBits, uint64_t thisBits, uint32_t argc,
                                        const uint64_t* argvBits);

extern "C" uint64_t bronze_create_func(bronze_fn_code code, int32_t param_count, uint64_t envBits) {
    return bronze_create_function(code, param_count >= 0 ? static_cast<uint32_t>(param_count) : 0,
                                  param_count >= 0 ? static_cast<uint32_t>(param_count) : 0,
                                  BRONZE_ABI_FN_NAME_NONE, BRONZE_ABI_FN_FLAGS_ORDINARY, envBits);
}

extern "C" uint64_t bronze_call_dynamic_0(uint64_t callee, uint64_t thisVal) {
    return bronze_dynamic_call(callee, thisVal, 0, nullptr);
}

extern "C" uint64_t bronze_call_dynamic_1(uint64_t callee, uint64_t thisVal, uint64_t a0) {
    uint64_t args[1] = {a0};
    return bronze_dynamic_call(callee, thisVal, 1, args);
}

extern "C" uint64_t bronze_call_dynamic_2(uint64_t callee, uint64_t thisVal, uint64_t a0, uint64_t a1) {
    uint64_t args[2] = {a0, a1};
    return bronze_dynamic_call(callee, thisVal, 2, args);
}

extern "C" uint64_t bronze_call_dynamic_3(uint64_t callee, uint64_t thisVal, uint64_t a0, uint64_t a1, uint64_t a2) {
    uint64_t args[3] = {a0, a1, a2};
    return bronze_dynamic_call(callee, thisVal, 3, args);
}

extern "C" uint64_t bronze_call_dynamic_4(uint64_t callee, uint64_t thisVal, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t args[4] = {a0, a1, a2, a3};
    return bronze_dynamic_call(callee, thisVal, 4, args);
}

extern "C" uint64_t bronze_call_dynamic_5(uint64_t callee, uint64_t thisVal, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    uint64_t args[5] = {a0, a1, a2, a3, a4};
    return bronze_dynamic_call(callee, thisVal, 5, args);
}

extern "C" uint64_t bronze_call_dynamic_6(uint64_t callee, uint64_t thisVal, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    uint64_t args[6] = {a0, a1, a2, a3, a4, a5};
    return bronze_dynamic_call(callee, thisVal, 6, args);
}

extern "C" uint64_t bronze_call_dynamic_7(uint64_t callee, uint64_t thisVal, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    uint64_t args[7] = {a0, a1, a2, a3, a4, a5, a6};
    return bronze_dynamic_call(callee, thisVal, 7, args);
}

extern "C" uint64_t bronze_call_dynamic_8(uint64_t callee, uint64_t thisVal, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    uint64_t args[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
    return bronze_dynamic_call(callee, thisVal, 8, args);
}

extern "C" uint64_t bronze_call_dynamic_n(uint64_t callee, uint64_t thisVal, uint32_t argc, const uint64_t* argv) {
    return bronze_dynamic_call(callee, thisVal, argc, argv);
}

extern "C" uint64_t bronze_super_call(uint64_t baseBits, uint64_t thisBits, uint32_t argc, const uint64_t* argvBits);

extern "C" uint64_t bronze_super_call_0(uint64_t base, uint64_t thisVal) {
    return bronze_super_call(base, thisVal, 0, nullptr);
}

extern "C" uint64_t bronze_super_call_1(uint64_t base, uint64_t thisVal, uint64_t a0) {
    uint64_t args[1] = {a0};
    return bronze_super_call(base, thisVal, 1, args);
}

extern "C" uint64_t bronze_super_call_2(uint64_t base, uint64_t thisVal, uint64_t a0, uint64_t a1) {
    uint64_t args[2] = {a0, a1};
    return bronze_super_call(base, thisVal, 2, args);
}

extern "C" uint64_t bronze_super_call_3(uint64_t base, uint64_t thisVal, uint64_t a0, uint64_t a1, uint64_t a2) {
    uint64_t args[3] = {a0, a1, a2};
    return bronze_super_call(base, thisVal, 3, args);
}

extern "C" uint64_t bronze_super_call_4(uint64_t base, uint64_t thisVal, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t args[4] = {a0, a1, a2, a3};
    return bronze_super_call(base, thisVal, 4, args);
}

extern "C" uint64_t bronze_super_call_5(uint64_t base, uint64_t thisVal, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    uint64_t args[5] = {a0, a1, a2, a3, a4};
    return bronze_super_call(base, thisVal, 5, args);
}

extern "C" uint64_t bronze_super_call_6(uint64_t base, uint64_t thisVal, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    uint64_t args[6] = {a0, a1, a2, a3, a4, a5};
    return bronze_super_call(base, thisVal, 6, args);
}

extern "C" uint64_t bronze_super_call_7(uint64_t base, uint64_t thisVal, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    uint64_t args[7] = {a0, a1, a2, a3, a4, a5, a6};
    return bronze_super_call(base, thisVal, 7, args);
}

extern "C" uint64_t bronze_super_call_8(uint64_t base, uint64_t thisVal, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    uint64_t args[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
    return bronze_super_call(base, thisVal, 8, args);
}

extern "C" uint64_t bronze_super_call_n(uint64_t base, uint64_t thisVal, uint32_t argc, const uint64_t* argv) {
    return bronze_super_call(base, thisVal, argc, argv);
}

extern "C" uint64_t bronze_construct_0(uint64_t callee) {
    return bronze_construct(callee, 0, nullptr);
}

extern "C" uint64_t bronze_construct_1(uint64_t callee, uint64_t a0) {
    uint64_t args[1] = {a0};
    return bronze_construct(callee, 1, args);
}

extern "C" uint64_t bronze_construct_2(uint64_t callee, uint64_t a0, uint64_t a1) {
    uint64_t args[2] = {a0, a1};
    return bronze_construct(callee, 2, args);
}

extern "C" uint64_t bronze_construct_3(uint64_t callee, uint64_t a0, uint64_t a1, uint64_t a2) {
    uint64_t args[3] = {a0, a1, a2};
    return bronze_construct(callee, 3, args);
}

extern "C" uint64_t bronze_construct_4(uint64_t callee, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t args[4] = {a0, a1, a2, a3};
    return bronze_construct(callee, 4, args);
}

extern "C" uint64_t bronze_construct_5(uint64_t callee, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    uint64_t args[5] = {a0, a1, a2, a3, a4};
    return bronze_construct(callee, 5, args);
}

extern "C" uint64_t bronze_construct_6(uint64_t callee, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    uint64_t args[6] = {a0, a1, a2, a3, a4, a5};
    return bronze_construct(callee, 6, args);
}

extern "C" uint64_t bronze_construct_7(uint64_t callee, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    uint64_t args[7] = {a0, a1, a2, a3, a4, a5, a6};
    return bronze_construct(callee, 7, args);
}

extern "C" uint64_t bronze_construct_8(uint64_t callee, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    uint64_t args[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
    return bronze_construct(callee, 8, args);
}

extern "C" uint64_t bronze_exception_get() {
    return bronze::runtime::rtTls()->exception_cell;
}

extern "C" void bronze_exception_set(uint64_t bits) {
    bronze::runtime::rtTls()->exception_cell = bits;
}

extern "C" uint64_t bronze_exception_take() {
    uint64_t bits = bronze::runtime::rtTls()->exception_cell;
    bronze::runtime::rtTls()->exception_cell = BRONZE_ABI_NO_EXCEPTION_BITS;
    return bits;
}

extern "C" int32_t bronze_exception_pending() {
    return bronze::runtime::rtTls()->exception_cell != BRONZE_ABI_NO_EXCEPTION_BITS;
}

extern "C" void bronze_register_key_manifest(const uint8_t* data) {
    if (!data) return;
    uint32_t count = 0;
    std::memcpy(&count, data, sizeof(uint32_t));
    const char* ptr = reinterpret_cast<const char*>(data + sizeof(uint32_t));
    for (uint32_t i = 0; i < count; ++i) {
        bronze_register_key_string(ptr);
        ptr += std::strlen(ptr) + 1;
    }
}

}  // namespace bronze::runtime

