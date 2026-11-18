#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "abi/bronze_abi.h"
#include "runtime/atomic_ref.h"
#include "runtime/bigint.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/host_globals.h"
#include "runtime/iterator.h"
#include "runtime/module_registry.h"
#include "runtime/type_feedback.h"
#include "runtime/object.h"
#include "runtime/promise.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/typed_array.h"
#include "runtime/realm.h"
#include "runtime/value.h"
#include "runtime/weak_ref.h"

namespace bronze::runtime {

static std::mutex g_nativeDisplayNamesMu;
static std::unordered_map<const void*, std::string> g_nativeDisplayNames;

static const void* resolveJumpThunk(const void* p) {
    if (!p) return nullptr;
#if defined(_WIN32) && defined(_M_X64)
    const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
    if (b[0] == 0xE9) {
        int32_t rel = *reinterpret_cast<const int32_t*>(b + 1);
        return b + 5 + rel;
    }
#endif
    return p;
}

void rtRegisterNativeDisplayName(const void* code, const char* displayName) {
    if (!code || !displayName) return;
    std::lock_guard<std::mutex> lock(g_nativeDisplayNamesMu);
    g_nativeDisplayNames[code] = displayName;
    const void* target = resolveJumpThunk(code);
    if (target && target != code) {
        g_nativeDisplayNames[target] = displayName;
    }
}

const char* rtGetNativeDisplayName(const void* code) {
    if (!code) return nullptr;
    std::lock_guard<std::mutex> lock(g_nativeDisplayNamesMu);
    auto it = g_nativeDisplayNames.find(code);
    if (it != g_nativeDisplayNames.end()) {
        return it->second.c_str();
    }
    const void* target = resolveJumpThunk(code);
    if (target && target != code) {
        it = g_nativeDisplayNames.find(target);
        if (it != g_nativeDisplayNames.end()) {
            return it->second.c_str();
        }
    }
    return nullptr;
}

bool rtResolveBuiltinGlobal(const std::string& keyStr, Value& out) {
    (void)rtHeap();
    if (keyStr == "Math") {
        out = rtMathObject();
    } else if (keyStr == "Object") {
        out = rtObjectNamespace();
    } else if (keyStr == "Function") {
        out = rtFunctionConstructorObject();
    } else if (keyStr == "JSON") {
        out = rtJsonNamespace();
    } else if (keyStr == "globalThis") {
        out = rtGlobalThisObject();
    } else if (keyStr == "performance") {
        out = rtPerformanceNamespace();
    } else if (keyStr == "Reflect") {
        out = rtReflectNamespace();
    } else if (keyStr == "Date") {
        out = rtDateConstructor();
    } else if (keyStr == "Intl") {
        out = rtIntlNamespace();
    } else if (keyStr == "Symbol") {
        out = rtSymbolFunction();
    } else if (Value bigint = rtBigIntConstructor(keyStr); bigint.isObject()) {
        out = bigint;
    } else if (Value regexp = rtRegExpConstructor(keyStr); regexp.isObject()) {
        out = regexp;
    } else if (Value iterator = rtIteratorConstructor(keyStr); iterator.isObject()) {
        out = iterator;
    } else if (Value collection = rtMapConstructor(keyStr); collection.isObject()) {
        out = collection;
    } else if (Value weak = rtWeakCollectionConstructor(keyStr); weak.isObject()) {
        out = weak;
    } else if (Value weakRef = rtWeakRefConstructor(keyStr); weakRef.isObject()) {
        out = weakRef;
    } else if (Value typed = rtTypedArrayConstructor(keyStr); typed.isObject()) {
        out = typed;
    } else if (Value shared = rtSharedArrayBufferConstructor(keyStr); shared.isObject()) {
        out = shared;
    } else if (keyStr == "Atomics") {
        out = rtAtomicsObject();
    } else if (Value dataView = rtDataViewConstructor(keyStr); dataView.isObject()) {
        out = dataView;
    } else if (Value global = rtGlobalConstructor(keyStr); global.isObject()) {
        out = global;
    } else if (Value ctor = rtErrorConstructor(keyStr); ctor.isObject()) {
        out = ctor;
    } else if (Value promise = rtPromiseConstructor(keyStr); promise.isObject()) {
        out = promise;
    } else if (Value numeric = rtGlobalNumericFunction(keyStr); numeric.isObject()) {
        out = numeric;
    } else if (Value moduleIntrinsic = rtModuleRegistryIntrinsic(keyStr);
               moduleIntrinsic.isObject()) {
        out = moduleIntrinsic;
    } else if (Value feedback = rtTypeFeedbackIntrinsic(keyStr); feedback.isObject()) {
        out = feedback;
    } else {
        return false;
    }
    return true;
}

Value rtGlobalThisObject() {
    return bronze::rtCurrentRealm()->globalObject();
}

bool rtGlobalThisOwnLookup(const std::string& name, Value& out) {
    Rooted<Value> globRoot{rtGlobalThisObject()};
    if (!globRoot.get().isObject()) return false;
    Rooted<Value> key{rtMakeString(name)};
    PropertyKey pkey = PropertyKey::fromValue(key.get());
    auto* obj = globRoot.get().asObject<ObjectHeader>();
    uint32_t slot = 0;
    if (!obj->shape || !obj->shape->lookupProperty(pkey, slot)) return false;
    out = globRoot.get().asObject<ObjectHeader>()->getProp(rtHeap(), key);
    return true;
}

// Set once a program has written a builtin's binding on a global object of
// this thread. Until then every such property still holds the builtin it was
// populated with, so the name's answer is the builtin's and the lookup below
// — an allocation and a shape walk, on a path `bronze_global_get` takes per
// read — is skipped.
static thread_local bool t_builtinBindingWritten = false;

bool rtGlobalObjectBinding(const std::string& name, Value& out, bool& cacheable) {
    if (!t_builtinBindingWritten) return false;
    Rooted<Value> globRoot{rtGlobalThisObject()};
    if (!globRoot.get().isObject()) return false;
    Rooted<Value> key{rtMakeString(name)};
    const PropertyKey pkey = PropertyKey::fromValue(key.get());
    auto* obj = globRoot.get().asObject<ObjectHeader>();
    PropertyInfo info;
    if (!obj->shape || !obj->shape->lookupProperty(pkey, info)) return false;
    cacheable = !info.accessor;
    out = globRoot.get().asObject<ObjectHeader>()->getProp(rtHeap(), key);
    return true;
}

// Never shrunk: a destroyed realm's root is an immortal shape no live object
// is built on, so keeping it costs a compare and never an answer.
thread_local std::vector<const Shape*> t_globalObjectRoots;

void rtWatchGlobalObjectRoot(const Shape* root) {
    for (const Shape* watched : t_globalObjectRoots) {
        if (watched == root) return;
    }
    t_globalObjectRoots.push_back(root);
}

void rtNoteGlobalObjectWrite(const ObjectHeader* obj, PropertyKey key) {
    if (!key.isString()) return;
    if (!rtRealmOfGlobal(Value::fromObject(const_cast<ObjectHeader*>(obj)))) return;
    const StringHeader* str = key.string();
    if (!str->isLatin1()) return;
    const std::string_view name(str->latin1Data(), str->getLength());
    // A host global the page wrote or defined is read from the global object
    // in this realm from now on (host_globals.h) — `performance` and `Intl`,
    // which are builtin names too, included.
    rtNoteHostGlobalOverridden(Value::fromObject(const_cast<ObjectHeader*>(obj)), name);
    for (const char* builtin : rtBuiltinGlobalNames()) {
        if (name == builtin) {
            t_builtinBindingWritten = true;
            rtInvalidateGlobalReadCells();
            return;
        }
    }
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
        bool cacheable = false;
        rtGlobalObjectBinding(name, resolved, cacheable);
        return resolved.rawBits();
    }
    if (rtGlobalThisOwnLookup(name, resolved)) {
        return resolved.rawBits();
    }
    return BRONZE_ABI_UNDEFINED_BITS;
}

// A write to a name no scope declared (lower_unresolved.cpp emitUnresolvedPut):
// 9.1.1.4.5 SetMutableBinding on the global environment, whose object record
// is the global object. A name the object has (or inherits, or a host
// registered) is Set there, refused as any Set is; a name it lacks is
// created by sloppy code — `Top = 1` makes `globalThis.Top`, configurable
// and deletable like any property a Set adds — and is 6.2.5.6 PutValue
// step 3's ReferenceError in strict code.
extern "C" void bronze_put_unresolved(uint64_t nameBits, uint64_t valBits, bool strict) {
    if (rtCachedGlobalWrite(Value(nameBits), Value(valBits))) return;
    Rooted<Value> name{Value(nameBits)};
    Rooted<Value> val{Value(valBits)};
    Rooted<Value> glob{rtGlobalThisObject()};
    if (strict) {
        const std::string text = rtAsciiChars(name.get().asString<StringHeader>());
        Value host = Value::fromUndefined();
        if (!rtHostGlobalLookup(text, host) && !bronze_has_property(name.get().rawBits(), glob.get().rawBits())) {
            rtThrowReferenceError(text + " is not defined");
            return;
        }
    }
    bronze_elem_set(glob.get().rawBits(), name.get().rawBits(), val.get().rawBits(), strict);
    // Where the property now lives, when it is an own writable data property
    // (a throw or a setter leaves none to record).
    rtRememberGlobalWrite(name.get());
}

// 10.2.1.2 OrdinaryCallBindThis steps 5-6 for a sloppy function whose
// receiver is not an object (lower_function.cpp emitSloppyThis): null and
// undefined are the global object, any other primitive its wrapper.
extern "C" uint64_t bronze_sloppy_this(uint64_t thisBits) {
    const Value received(thisBits);
    if (received.isNull() || received.isUndefined()) return rtGlobalThisObject().rawBits();
    if (received.isObject()) return thisBits;
    return rtToObject(received).rawBits();
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

// A call generated code could not enter itself (il_call_entry.cpp); argv is
// the caller's rooted argv block.
extern "C" uint64_t bronze_call_dynamic_n(uint64_t callee, uint64_t thisVal, uint32_t argc, const uint64_t* argv) {
    return bronze_dynamic_call(callee, thisVal, argv ? argc : 0, argv);
}

extern "C" uint64_t bronze_super_call(uint64_t activeBits, uint64_t thisBits, uint32_t argc, const uint64_t* argvBits);

// super() the inline path did not enter (il_call_entry.cpp lower_super_call_inline);
// argv is the caller's rooted argv block.
extern "C" uint64_t bronze_super_call_n(uint64_t active, uint64_t thisVal, uint32_t argc, const uint64_t* argv) {
    return bronze_super_call(active, thisVal, argv ? argc : 0, argv);
}

extern "C" void bronze_register_key_manifest(const uint8_t* data, uint32_t* key_map) {
    if (!data) return;
    uint32_t count = 0;
    std::memcpy(&count, data, sizeof(uint32_t));
    const uint8_t* ptr = data + sizeof(uint32_t);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t len = 0;
        std::memcpy(&len, ptr, sizeof(uint32_t));
        ptr += sizeof(uint32_t);
        uint32_t id = bronze_register_key_string_len(reinterpret_cast<const char*>(ptr), len);
        // The remap is shared by every thread that runs the module (it sits
        // outside the per-thread instance run, bronze_module_instance), and
        // interning is process-wide, so a second thread's entry computes the
        // same ids: written only when different, and atomically, so a thread
        // already reading the map never meets a torn or racing store.
        if (key_map) {
            AtomicRef<uint32_t> cell(key_map[i]);
            if (cell.load(std::memory_order_relaxed) != id) cell.store(id, std::memory_order_relaxed);
        }
        ptr += len + 1;
    }
}

extern "C" double bronze_f64_mod(double a, double b) {
    return std::fmod(a, b);
}

}  // namespace bronze::runtime

