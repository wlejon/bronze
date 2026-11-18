// Global bare-name lookup and per-module global read caches.
//
// All mutable per-thread storage stays owned by rt_state.cpp (reached through
// rtThreadModuleState / rtStackModuleEpoch) so this translation unit introduces
// no thread_local destructors of its own.

#include <algorithm>
#include <cstdint>
#include <string>

#include "runtime/heap.h"
#include "runtime/tls_block.h"

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
// and on every change to a global object's table generated code reads through
// (Dictionary::notifiesGlobalReads), so the per-realm cache, its inline cells
// and the module cells retire together. It lives in the TLS block, where
// generated code compares a cell's with it.
#define t_globalReadEpoch (rtTls()->global_read_epoch)

void rtBumpGlobalReadEpoch() noexcept { ++rtTls()->global_read_epoch; }

void rtForgetGlobalReadCells() noexcept {
    rtTls()->global_read_cells = 0;
    rtTls()->global_read_cell_count = 0;
}

namespace {

// The word every name cached absent reads through: the hole.
const uint64_t kAbsentWord = BRONZE_ABI_HOLE_BITS;

// Publishes the realm's table to generated code (it is the current realm's:
// every fill is) and makes `e`'s cell valid, reading through `read` and, when
// not null, writing through `write`.
void fillInline(Realm* realm, Realm::GlobalReadEntry& e, const uint64_t* read, uint64_t* write) {
    auto& reads = realm->globalReads();
    bronze_tls_block* tls = rtTls();
    tls->global_read_cells = reinterpret_cast<uint64_t>(reads.data());
    tls->global_read_cell_count = reads.size();
    e.inlineRead = read;
    e.inlineWrite = write;
    e.inlineEpoch = tls->global_read_epoch;
}

// Whether generated code may keep the address of the global object's slot
// `slot`: the object's table tells it of every change (and is marked to), and
// the storage the slot lives in is old, so no collection moves it.
// A shape-chain global object's every new shape moves the epoch
// (rtNoteGlobalShapeChange); a dictionary-mode one's table is marked to; and
// every collection moves it (heap.cpp), since a young object or slot block
// moves only then.
uint64_t* stableSlot(ObjectHeader* obj, uint32_t slot) {
    if (!obj->shape || !rtMaybeGlobalObjectShape(obj->shape)) return nullptr;
    uint64_t* addr = nullptr;
    if (slot < ObjectHeader::kOverflowSlot) {
        addr = reinterpret_cast<uint64_t*>(&obj->slotsData()[slot]);
    } else {
        auto* block = obj->overflow.asObject<HeapObjectHeader>();
        addr = reinterpret_cast<uint64_t*>(&block->payload<Value>()[slot - ObjectHeader::kOverflowSlot]);
    }
    if (obj->shape->isDictionary()) obj->shape->dict->notifiesGlobalReads = true;
    return addr;
}

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
    if (e.kind != 2 && e.kind != 3) return false;
    const Value glob = realm->globalObject();
    if (!glob.isObject()) return false;
    const auto* obj = glob.asObject<ObjectHeader>();
    if (obj->shape != e.shape) return false;
    if (obj->shape->isDictionary() && obj->shape->dict->generation != e.generation) return false;
    if (e.kind == 3) {
        // Absent: still so while no property was added (a dictionary's add
        // keeps its generation and grows its entries; a shape's is a new
        // shape).
        if (obj->shape->isDictionary() && obj->shape->dict->entries.size() != e.slot) return false;
        out = Value::fromHole().rawBits();
        return true;
    }
    out = obj->getSlot(e.slot).rawBits();
    return true;
}

Realm::GlobalReadEntry& readEntry(Realm* realm, uint32_t keyIndex) {
    auto& reads = realm->globalReads();
    if (keyIndex >= reads.size()) {
        // A move of the table moves the values cells read through.
        const auto* before = reads.data();
        reads.resize(std::max<size_t>(static_cast<size_t>(keyIndex) + 1, reads.size() * 2));
        if (reads.data() != before) {
            rtBumpGlobalReadEpoch();
            rtForgetGlobalReadCells();
        }
    }
    return reads[keyIndex];
}

void rememberValue(Realm* realm, uint32_t keyIndex, Value v) {
    Realm::GlobalReadEntry& e = readEntry(realm, keyIndex);
    e.epoch = t_globalReadEpoch;
    e.kind = 1;
    e.value = v;
    fillInline(realm, e, reinterpret_cast<const uint64_t*>(&e.value), nullptr);
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
    auto* mutableObj = const_cast<ObjectHeader*>(obj);
    if (uint64_t* addr = stableSlot(mutableObj, info.slot)) {
        const bool frozen =
            obj->shape->isDictionary() && obj->shape->dict->level == IntegrityLevel::Frozen;
        fillInline(realm, e, addr, e.writable && !frozen ? addr : nullptr);
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

void rtRememberGlobalAbsent(uint32_t keyIndex) {
    // A name the host provides (a lazy one among them, whose init runs on its
    // first read) is never absent: the host answers it, and a host that has
    // not yet (an accessor whose getter runs elsewhere, a lazy init that
    // registers late) must be asked again.
    if (rtHostGlobalRegistered(rtKeyString(keyIndex))) return;
    Realm* realm = rtCurrentRealm();
    const Value glob = realm->globalObject();
    if (!glob.isObject()) return;
    const auto* obj = glob.asObject<ObjectHeader>();
    if (!obj->shape) return;
    Realm::GlobalReadEntry& e = readEntry(realm, keyIndex);
    e.epoch = t_globalReadEpoch;
    e.kind = 3;
    e.shape = obj->shape;
    const bool dict = obj->shape->isDictionary();
    e.generation = dict ? obj->shape->dict->generation : 0;
    e.slot = dict ? static_cast<uint32_t>(obj->shape->dict->entries.size()) : 0;
    e.writable = false;
    e.value = Value::fromUndefined();
    if (rtMaybeGlobalObjectShape(obj->shape)) {
        if (dict) obj->shape->dict->notifiesGlobalReads = true;
        fillInline(realm, e, &kAbsentWord, nullptr);
    }
}

void rtRememberGlobalValue(uint32_t keyIndex, Value v) {
    rememberValue(rtCurrentRealm(), keyIndex, v);
}

void rtRememberGlobalSlot(uint32_t keyIndex) { rememberSlot(rtCurrentRealm(), keyIndex); }

extern "C" {

uint64_t bronze_global_get(uint32_t keyIndex, uint64_t* cacheCell) {
    recordPropCall("bronze_global_get", keyIndex, nullptr);
    Realm* realm = rtCurrentRealm();
    if (uint64_t cached = 0; cachedGlobalRead(realm, keyIndex, cached)) {
        if (Value(cached).isHole()) {
            return rtThrowReferenceError(rtKeyString(keyIndex) + " is not defined").rawBits();
        }
        return cached;
    }
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
