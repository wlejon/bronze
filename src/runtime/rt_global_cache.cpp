// Global bare-name lookup and per-module global read caches.
//
// All mutable per-thread storage stays owned by rt_state.cpp (reached through
// rtThreadModuleState / rtStackModuleEpoch) so this translation unit introduces
// no thread_local destructors of its own.

#include <cstdint>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/host_globals.h"
#include "runtime/profile.h"
#include "runtime/realm.h"
#include "runtime/root_remset.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_module_state.h"
#include "runtime/rt_property.h"
#include "runtime/object.h"
#include "runtime/property_key.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/value.h"

namespace bronze::runtime {

// The global-read epoch: a cached answer (Realm::globalReads) holds while its
// epoch is this one. Bumped wherever the module read cells are emptied — a
// host global registered, overridden or forced, a builtin's binding written —
// so the per-realm cache and the cells retire together.
static thread_local uint64_t t_globalReadEpoch = 1;

void rtBumpGlobalReadEpoch() noexcept { ++t_globalReadEpoch; }

namespace {

// Reads the global object's own data property `keyIndex` straight from its
// slot when the realm's entry for the key still describes the object.
bool cachedGlobalRead(Realm* realm, uint32_t keyIndex, uint64_t& out) {
    auto& reads = realm->globalReads();
    if (keyIndex >= reads.size()) return false;
    const Realm::GlobalReadEntry& e = reads[keyIndex];
    if (e.epoch != t_globalReadEpoch) return false;
    if (e.kind == 1) {
        out = e.value.rawBits();
        return true;
    }
    if (e.kind != 2) return false;
    const Value glob = realm->globalObject();
    if (!glob.isObject()) return false;
    const auto* obj = glob.asObject<ObjectHeader>();
    if (obj->shape != e.shape) return false;
    if (obj->shape->isDictionary() && obj->shape->dict->generation != e.generation) return false;
    out = obj->getSlot(e.slot).rawBits();
    return true;
}

Realm::GlobalReadEntry& readEntry(Realm* realm, uint32_t keyIndex) {
    auto& reads = realm->globalReads();
    if (keyIndex >= reads.size()) reads.resize(static_cast<size_t>(keyIndex) + 1);
    return reads[keyIndex];
}

void rememberValue(Realm* realm, uint32_t keyIndex, Value v) {
    Realm::GlobalReadEntry& e = readEntry(realm, keyIndex);
    e.epoch = t_globalReadEpoch;
    e.kind = 1;
    e.value = v;
}

// The global object's own property `keyIndex`, when it is a boxed data
// property: where it lives, for cachedGlobalRead.
void rememberSlot(Realm* realm, uint32_t keyIndex) {
    const Value glob = realm->globalObject();
    StringHeader* name = rtKeyHeader(keyIndex);
    if (!glob.isObject() || name == nullptr) return;
    const auto* obj = glob.asObject<ObjectHeader>();
    if (!obj->shape) return;
    PropertyInfo info;
    if (!obj->shape->lookupProperty(PropertyKey::fromValue(Value::fromString(name)), info)) return;
    if (info.accessor || info.repr != SlotRepr::Boxed) return;
    Realm::GlobalReadEntry& e = readEntry(realm, keyIndex);
    e.epoch = t_globalReadEpoch;
    e.kind = 2;
    e.shape = obj->shape;
    e.generation = obj->shape->isDictionary() ? obj->shape->dict->generation : 0;
    e.slot = info.slot;
    e.value = Value::fromUndefined();
    e.writable = false;
    if (info.writable) {
        const std::string& text = rtKeyString(keyIndex);
        bool special = realm->hostGlobalNames().count(text) != 0;
        for (const char* builtin : rtBuiltinGlobalNames()) {
            if (special) break;
            special = text == builtin;
        }
        e.writable = !special;
    }
}

// The key id of `name` when it is the arena header of a registered key — what
// a compiled bare-name write passes — else false. Arena headers never move,
// so the pointer is the map's key.
bool keyIndexOfHeader(const StringHeader* name, uint32_t& out) {
    static thread_local std::unordered_map<const StringHeader*, uint32_t> t_keyOfHeader;
    if (const auto it = t_keyOfHeader.find(name); it != t_keyOfHeader.end()) {
        out = it->second;
        return true;
    }
    if (!name->isLatin1()) return false;
    uint32_t index = 0;
    if (!rtKeyIndexOf(std::string_view(name->latin1Data(), name->getLength()), index)) return false;
    if (rtKeyHeader(index) != name) return false;
    t_keyOfHeader.emplace(name, index);
    out = index;
    return true;
}

}  // namespace

bool rtCachedGlobalWrite(Value name, Value val) {
    if (!name.isString()) return false;
    uint32_t keyIndex = 0;
    if (!keyIndexOfHeader(name.asString<StringHeader>(), keyIndex)) return false;
    Realm* realm = rtCurrentRealm();
    auto& reads = realm->globalReads();
    if (keyIndex < reads.size()) {
        const Realm::GlobalReadEntry& e = reads[keyIndex];
        if (e.epoch == t_globalReadEpoch && e.kind == 2 && e.writable) {
            const Value glob = realm->globalObject();
            if (glob.isObject()) {
                auto* obj = glob.asObject<ObjectHeader>();
                const Shape* shape = obj->shape;
                if (shape == e.shape &&
                    (!shape->isDictionary() || (shape->dict->generation == e.generation &&
                                                shape->dict->level != IntegrityLevel::Frozen))) {
                    obj->setSlot(e.slot, val);
                    return true;
                }
            }
        }
    }
    return false;
}

void rtRememberGlobalWrite(Value name) {
    if (!name.isString()) return;
    uint32_t keyIndex = 0;
    if (!keyIndexOfHeader(name.asString<StringHeader>(), keyIndex)) return;
    rememberSlot(rtCurrentRealm(), keyIndex);
}

bool rtCachedGlobalRead(uint32_t keyIndex, uint64_t& out) {
    return cachedGlobalRead(rtCurrentRealm(), keyIndex, out);
}

void rtRememberGlobalValue(uint32_t keyIndex, Value v) {
    rememberValue(rtCurrentRealm(), keyIndex, v);
}

void rtRememberGlobalSlot(uint32_t keyIndex) { rememberSlot(rtCurrentRealm(), keyIndex); }

extern "C" {

uint64_t bronze_global_get(uint32_t keyIndex, uint64_t* cacheCell) {
    recordPropCall("bronze_global_get", keyIndex, nullptr);
    Realm* realm = rtCurrentRealm();
    if (uint64_t cached = 0; cachedGlobalRead(realm, keyIndex, cached)) return cached;
    const std::string& keyStr = rtKeyString(keyIndex);
    if (keyStr == "globalThis") {
        return rtGlobalThisObject().rawBits();
    }
    if (rtHostShadowsBuiltin(keyStr)) {
        if (Value host = Value::fromUndefined(); rtHostGlobalLookup(keyStr, host)) {
            rememberValue(realm, keyIndex, host);
            return host.rawBits();
        }
        // The host's name, overridden by the page (host_globals.h): the
        // global object's property, never the builtin it shadowed.
        if (rtHostGlobalRegistered(keyStr)) {
            if (Value own = Value::fromUndefined(); rtGlobalThisOwnLookup(keyStr, own)) {
                rememberSlot(rtCurrentRealm(), keyIndex);
                return own.rawBits();
            }
            return rtThrowReferenceError(keyStr + " is not defined").rawBits();
        }
    }
    Value resolved = Value::fromUndefined();
    if (!rtResolveBuiltinGlobal(keyStr, resolved)) {
        if (Value host = Value::fromUndefined(); rtHostGlobalLookup(keyStr, host)) {
            rememberValue(realm, keyIndex, host);
            return host.rawBits();
        }
        if (Value fromGlobalObject = Value::fromUndefined();
            rtGlobalThisOwnLookup(keyStr, fromGlobalObject)) {
            // A getter may have run; the realm is the same either way.
            rememberSlot(rtCurrentRealm(), keyIndex);
            return fromGlobalObject.rawBits();
        }
        // A provided name the page deleted (`delete window.screen`): the bare
        // read is the ReferenceError a missing global is.
        return rtThrowReferenceError(keyStr + " is not defined").rawBits();
    }
    bool cacheable = true;
    const bool fromObject = rtGlobalObjectBinding(keyStr, resolved, cacheable);
    if (cacheCell && cacheable) {
        *cacheCell = resolved.rawBits();
        rtNoteRootCellWrite(cacheCell);
    }
    if (cacheable) {
        if (fromObject) rememberSlot(realm, keyIndex);
        else rememberValue(realm, keyIndex, resolved);
    }
    return resolved.rawBits();
}

}  // extern "C"

void rtInvalidateGlobalReadCells() {
    rtBumpGlobalReadEpoch();
    for (const auto& span : rtThreadModuleState().globalCacheSpans) {
        for (uint64_t i = 0; i < span.count; ++i) span.cells[i] = Value(BRONZE_ABI_HOLE_BITS);
    }
}

extern "C" {

uint64_t bronze_global_get_cached(uint32_t keyIndex, uint64_t* cells, uint64_t count,
                                  uint32_t slot) {
    recordPropCall("bronze_global_get_cached", keyIndex, nullptr);
    ThreadModuleState& s = rtThreadModuleState();
    auto* valueCells = reinterpret_cast<Value*>(cells);
    if (cells && count != 0 && slot < count) {
        bool known = false;
        for (const auto& span : s.globalCacheSpans) {
            if (span.cells == valueCells) {
                known = true;
                break;
            }
        }
        if (!known) {
            uint64_t spanEpoch = rtStackModuleEpoch();
            for (const auto* spans : {&s.moduleValueCells, &s.moduleRememberedCells}) {
                for (const auto& vspan : *spans) {
                    if (vspan.cells + vspan.count == valueCells) {
                        spanEpoch = vspan.epoch;
                        break;
                    }
                }
            }
            s.globalCacheSpans.push_back({valueCells, count, spanEpoch});
            moduleRemsetSpanAdded(s, s.globalCacheSpans.back());
        }
    }
    const std::string& keyStr = rtKeyString(keyIndex);
    Value resolved = Value::fromUndefined();
    bool cacheable = false;
    if (keyStr == "globalThis") {
        resolved = rtGlobalThisObject();
        cacheable = false;
    } else if (rtHostShadowsBuiltin(keyStr) && rtHostGlobalLookup(keyStr, resolved)) {
        cacheable = true;
    } else if (rtResolveBuiltinGlobal(keyStr, resolved)) {
        // The global object's property, which starts as the builtin; the cell
        // is emptied when a program writes it (rtNoteGlobalObjectWrite).
        cacheable = true;
        rtGlobalObjectBinding(keyStr, resolved, cacheable);
    } else if (rtHostGlobalLookup(keyStr, resolved)) {
        cacheable = true;
    } else if (!rtGlobalThisOwnLookup(keyStr, resolved)) {
        return rtThrowReferenceError(keyStr + " is not defined").rawBits();
    }
    if (cacheable && cells && slot < count) {
        valueCells[slot] = resolved;
        moduleRemsetNoteCell(s, cells + slot);
    }
    return resolved.rawBits();
}

}  // extern "C"

}  // namespace bronze::runtime
