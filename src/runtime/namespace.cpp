// The module namespace exotic object: how one is built, and the four questions
// a program can ask it.
//
// It is built from the object literal of getters `src/modules/link.cpp`
// synthesizes, rather than from a list the linker hands over directly. The
// reason is that the getters have to be real closures over the exporting
// module's bindings, and the one thing that can build a closure is the compiler
// — so the literal is lowered exactly as any other, and this converts the
// result. What the conversion adds is everything 10.4.6 says an ordinary object
// is not: a sorted key list, a [[Set]] that always refuses, and a descriptor
// that says `configurable: false`.
//
// The SORT is 10.4.6.2 and it is also the house determinism rule met head-on: a
// namespace's key order must be a function of the export names, and never of
// the order a shape's transition tree or a hash table happens to hold them in.
// `StringHeader::lessThan` is 7.2.13 IsStringLessThan — UTF-16 code unit by
// code unit — which is exactly the order 10.4.6.2 names.

#include "runtime/namespace.h"

#include <algorithm>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/accessor.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/promise.h"
#include "runtime/rt_internal.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

namespace bronze {

ModuleNamespaceHeader* ModuleNamespaceHeader::create(Heap& heap, uint32_t count) {
    const size_t payload =
        sizeof(ModuleNamespaceHeader) - sizeof(HeapObjectHeader) + 2 * size_t(count) * sizeof(Value);
    HeapObjectHeader* raw = heap.allocate(payload, Tag::Object);
    auto* ns = reinterpret_cast<ModuleNamespaceHeader*>(raw);
    ns->header.flags = kFlags;
    ns->count = count;
    ns->reserved = 0;
    for (uint32_t i = 0; i < 2 * count; ++i) ns->entries()[i] = Value::fromUndefined();
    return ns;
}

int32_t ModuleNamespaceHeader::indexOf(const StringHeader* key) const {
    if (!key) return -1;
    for (uint32_t i = 0; i < count; ++i) {
        Value stored = name(i);
        if (!stored.isString()) continue;
        if (stored.asString<StringHeader>()->equals(*key)) return static_cast<int32_t>(i);
    }
    return -1;
}

namespace runtime {

namespace {

ModuleNamespaceHeader* asNamespace(Value v) {
    if (!v.isObject()) return nullptr;
    auto* hdr = v.asObject<HeapObjectHeader>();
    if (hdr->flags != ModuleNamespaceHeader::kFlags) return nullptr;
    return reinterpret_cast<ModuleNamespaceHeader*>(hdr);
}

// One `import.meta` object per module, keyed by the URL constant lowering
// assigned. 16.2.1.10 caches the object on the Module Record, so every mention
// of `import.meta` inside one module is the SAME object and a property written
// on it is visible to the next mention; the key-constant index is that record's
// stand-in here, because lowering already made it one index per module.
//
// Rooted through a root SOURCE and not slot by slot: the table grows as the
// program's modules reach their first `import.meta`, and a growing vector
// reallocates, which is exactly the case `add_permanent_root` cannot describe
// (it pins one address). The list is bounded by the module graph, not by how
// many times the expression runs.
//
// A namespace-scope vector rather than a function-local static, and the same
// reason `ensureExceptionRoots` uses one: the visitor the collector calls reads
// this table, so a table whose first read is also its own initialization would
// be re-entered by a collection that happened inside that initialization.
std::vector<Value> g_importMetaObjects;

void ensureImportMetaRoots() {
    static const bool registered = [] {
        rtHeap().add_root_source([](const Heap::RootVisitor& visit) {
            for (Value& v : g_importMetaObjects) visit(v);
        });
        return true;
    }();
    (void)registered;
}

}  // namespace

bool rtIsModuleNamespace(Value v) { return asNamespace(v) != nullptr; }

std::vector<StringHeader*> rtModuleNamespaceKeys(Value nsVal) {
    std::vector<StringHeader*> out;
    ModuleNamespaceHeader* ns = asNamespace(nsVal);
    if (!ns) return out;
    out.reserve(ns->count);
    // Already in 10.4.6.2 order: the sort happened once, at construction, over
    // arena strings that never move. Nothing here can reorder them.
    for (uint32_t i = 0; i < ns->count; ++i) out.push_back(ns->name(i).asString<StringHeader>());
    return out;
}

// 10.4.6.4 [[HasProperty]], which is the whole of it: a namespace's
// [[Prototype]] is null (10.4.6.1), so there is no chain to continue on and
// being one of the export names is the only way to be true. It is the same
// `indexOf` [[Get]] and [[GetOwnProperty]] use rather than a second walk,
// because a second walk would be a second answer to what this object holds.
//
// No allocation, and no getter call: `in` asks whether the property is there,
// and running the binding's getter to find out would be a side effect the
// operator does not have.
bool rtModuleNamespaceHasExport(Value nsVal, const StringHeader* key) {
    ModuleNamespaceHeader* ns = asNamespace(nsVal);
    return ns != nullptr && ns->indexOf(key) >= 0;
}

bool rtModuleNamespaceGet(Value nsVal, const StringHeader* key, Value& out) {
    ModuleNamespaceHeader* ns = asNamespace(nsVal);
    if (!ns) return false;
    const int32_t at = ns->indexOf(key);
    // A name the module does not export is NOT an error: 10.4.6.7 step 3 falls
    // through to `undefined`, exactly as an ordinary object does for a name it
    // does not carry. `import { missing }` is the early error; `ns.missing` is
    // this.
    if (at < 0) {
        out = Value::fromUndefined();
        return true;
    }
    Rooted<Value> receiver{nsVal};
    // The getter is read before the call and through the header, which is safe
    // because nothing between here and `callGetter` allocates; `callGetter`
    // itself takes the receiver through a root, as its contract requires.
    Value getter = ns->getter(static_cast<uint32_t>(at));
    out = callGetter(getter, receiver);
    return true;
}

// 10.4.6.1: the one own SYMBOL-keyed property a namespace has, and the only own
// key of one that is not an export. It is a fixed data property — the string
// "Module", non-writable, non-enumerable, non-configurable — installed at
// creation in the specification; here it is answered rather than stored,
// because the object has no shape a property could go in and there is nothing
// about it that could ever differ between two namespaces.
bool rtModuleNamespaceOwnSymbol(Value nsVal, Value keyVal, Value& out) {
    if (!asNamespace(nsVal)) return false;
    if (!keyVal.isSymbol() || keyVal.asSymbol<SymbolHeader>() != rtSymbolToStringTag()) {
        return false;
    }
    out = rtMakeString("Module");
    return true;
}

bool rtModuleNamespaceOwnProperty(Value nsVal, Value keyVal, Value& outValue) {
    if (!asNamespace(nsVal)) return false;
    // 10.4.6.5 step 1: a SYMBOL falls through to OrdinaryGetOwnProperty, whose
    // whole answer for a namespace is the `@@toStringTag` above — asked for by
    // the caller, since only it knows whether it wants the descriptor's
    // attributes or its value.
    if (keyVal.isSymbol()) return false;
    Rooted<Value> self{nsVal};
    Rooted<Value> key{rtValueToString(keyVal)};
    if (rtExceptionPending()) return false;
    ModuleNamespaceHeader* ns = asNamespace(self.get());
    if (ns->indexOf(key.get().asString<StringHeader>()) < 0) return false;
    // Step 4 reads the value through [[Get]], which is what makes the
    // descriptor's `value` the binding's CURRENT one rather than the one
    // linking saw.
    return rtModuleNamespaceGet(self.get(), key.get().asString<StringHeader>(), outValue);
}

bool rtModuleNamespaceWriteRefused(Value nsVal, const std::string& key, bool strict) {
    if (!asNamespace(nsVal)) return false;
    // 10.4.6.9 [[Set]] returns false for EVERY key, whether or not the module
    // exports it, and 13.15.2 PutValue step 6.d turns a false from a strict
    // reference into a TypeError. Module code is always strict (11.2.2), so the
    // sloppy half of this is unreachable from a module — it is written anyway,
    // because a namespace object can be passed into a function bronze compiled
    // from a sloppy script and the refusal has to mean the same thing there.
    if (strict) {
        rtThrowTypeError("Cannot assign to read only property '" + key +
                         "' of a module namespace object");
    }
    return true;
}

extern "C" {

// The literal of getters in, the exotic object out. Called once per
// `import * as` binding, at the point the linker put the declaration, so
// nothing here is on a hot path and the sort costs what it costs.
uint64_t bronze_module_namespace(uint64_t sourceBits) {
    Value sourceVal(sourceBits);
    if (!sourceVal.isObject() ||
        sourceVal.asObject<HeapObjectHeader>()->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        fatal("internal: a module namespace built from something that is not an object literal");
    }
    Rooted<Value> source{sourceVal};

    // Phase one reads the shape only. The keys are arena-interned and immortal,
    // so they survive the allocation in phase two; the getters are ordinary
    // heap values and are therefore read AFTER it, off the re-derived object.
    std::vector<StringHeader*> keys =
        rtOwnStringKeysOrdered(source.get().asObject<ObjectHeader>(), /*enumerableOnly=*/false);
    // 10.4.6.2, and the whole reason this object exists rather than the literal.
    std::sort(keys.begin(), keys.end(),
              [](const StringHeader* a, const StringHeader* b) { return a->lessThan(*b); });

    const uint32_t count = static_cast<uint32_t>(keys.size());
    Rooted<Value> nsRoot{Value::fromObject(ModuleNamespaceHeader::create(rtHeap(), count))};

    auto* obj = source.get().asObject<ObjectHeader>();
    auto* ns = reinterpret_cast<ModuleNamespaceHeader*>(nsRoot.get().asObject<HeapObjectHeader>());
    for (uint32_t i = 0; i < count; ++i) {
        PropertyInfo info;
        if (!obj->shape || !obj->shape->lookupProperty(keys[i], info) || !info.accessor) {
            fatal("internal: a module namespace entry that is not a getter");
        }
        ns->entries()[2 * i] = Value::fromString(keys[i]);
        ns->entries()[2 * i + 1] = obj->getSlot(info.slot);
    }
    return nsRoot.get().rawBits();
}

// 16.2.1.10 `import.meta`. The object is OrdinaryObjectCreate(NULL) — no
// prototype, so `import.meta.toString` is `undefined` rather than
// `Object.prototype`'s, which is what makes "every property but `url` reads as
// undefined" true rather than nearly true.
//
// It is ORDINARY and extensible, not frozen: 16.2.1.10 step 4 hands it to the
// host to add properties to, and a program may add its own. What is fixed is
// its IDENTITY within a module, which the cache above provides.
uint64_t bronze_import_meta(uint32_t urlKeyIndex) {
    ensureImportMetaRoots();
    std::vector<Value>& cache = g_importMetaObjects;
    if (urlKeyIndex < cache.size() && cache[urlKeyIndex].isObject()) {
        return cache[urlKeyIndex].rawBits();
    }
    // Grown BEFORE the object is built, so the assignment at the end cannot be
    // the thing that reallocates — the root source walks the vector, so a
    // reallocation between building the object and storing it would be safe
    // anyway, but growing first also means the slot exists when it is needed.
    if (urlKeyIndex >= cache.size()) {
        cache.resize(static_cast<size_t>(urlKeyIndex) + 1, Value::fromUndefined());
    }

    Rooted<Value> meta{Value::fromObject(
        ObjectHeader::create(rtHeap(), rtArena(), rtRootShapeForPrototype(Value::fromNull())))};
    meta.get().asObject<ObjectHeader>()->header.flags = BRONZE_ABI_OBJ_FLAGS_PLAIN;
    Rooted<Value> key{rtMakeString("url")};
    Rooted<Value> url{rtMakeString(rtKeyString(urlKeyIndex))};
    meta.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, url);
    cache[urlKeyIndex] = meta.get();
    return meta.get().rawBits();
}

uint64_t bronze_dynamic_import(uint64_t specifierBits) {
    (void)specifierBits;
    Rooted<Value> promise{rtNewPromise()};
    Rooted<Value> err{Value::fromString(StringHeader::createFromUTF8(
        rtHeap(), "TypeError: Cannot resolve module"))};
    rtRejectPromise(promise, err);
    return promise.get().rawBits();
}

}  // extern "C"

}  // namespace runtime
}  // namespace bronze
