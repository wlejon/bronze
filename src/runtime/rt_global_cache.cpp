// Global bare-name lookup and per-module global read caches.
//
// All mutable per-thread storage stays owned by rt_state.cpp (reached through
// rtThreadModuleState / rtStackModuleEpoch) so this translation unit introduces
// no thread_local destructors of its own.

#include <cstdint>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/fatal.h"
#include "runtime/host_globals.h"
#include "runtime/profile.h"
#include "runtime/realm.h"
#include "runtime/root_remset.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_module_state.h"
#include "runtime/rt_property.h"
#include "runtime/rt_state.h"
#include "runtime/value.h"

namespace bronze::runtime {

extern "C" {

uint64_t bronze_global_get(uint32_t keyIndex, uint64_t* cacheCell) {
    recordPropCall("bronze_global_get", keyIndex, nullptr);
    const std::string& keyStr = rtKeyString(keyIndex);
    if (keyStr == "globalThis") {
        return rtGlobalThisObject().rawBits();
    }
    if (rtHostShadowsBuiltin(keyStr)) {
        if (Value host = Value::fromUndefined(); rtHostGlobalLookup(keyStr, host)) {
            return host.rawBits();
        }
    }
    Value resolved = Value::fromUndefined();
    if (!rtResolveBuiltinGlobal(keyStr, resolved)) {
        if (Value host = Value::fromUndefined(); rtHostGlobalLookup(keyStr, host)) {
            return host.rawBits();
        }
        if (Value fromGlobalObject = Value::fromUndefined();
            rtGlobalThisOwnLookup(keyStr, fromGlobalObject)) {
            return fromGlobalObject.rawBits();
        }
        fatal(("internal: no global named " + keyStr).c_str());
    }
    bool cacheable = true;
    rtGlobalObjectBinding(keyStr, resolved, cacheable);
    if (cacheCell && cacheable) {
        *cacheCell = resolved.rawBits();
        rtNoteRootCellWrite(cacheCell);
    }
    return resolved.rawBits();
}

}  // extern "C"

void rtInvalidateGlobalReadCells() {
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
        fatal(("internal: no global named " + keyStr).c_str());
    }
    if (cacheable && cells && slot < count) {
        valueCells[slot] = resolved;
        moduleRemsetNoteCell(s, cells + slot);
    }
    return resolved.rawBits();
}

}  // extern "C"

}  // namespace bronze::runtime
