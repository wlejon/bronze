#define _CRT_SECURE_NO_WARNINGS

#include "runtime/ic_log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/fn.h"
#include "runtime/map.h"
#include "runtime/namespace.h"
#include "runtime/object.h"
#include "runtime/regexp.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"
#include "runtime/weak_ref.h"

namespace bronze::runtime {

bool g_icLogEnabled = false;

namespace {

struct KeyStats {
    uint64_t totalCount{0};
    std::unordered_map<std::string, uint64_t> reasonCounts;
};

struct SectionStats {
    uint64_t totalCount{0};
    std::unordered_map<std::string, uint64_t> reasonCounts;
    std::unordered_map<std::string, KeyStats> keyStats;
};

static SectionStats g_propGetStats;
static SectionStats g_propSetStats;
static SectionStats g_dynamicCallStats;
// The computed-read cache's refusals and stale entries, by reason. A section
// with no key column: a miss here is attributed to the CACHE's state and to
// the receiver's kind, never to a key the site names — a computed site names
// no key, which is the whole reason the cache exists.
static SectionStats g_elemIcStats;
static bool s_initialized = false;

// A read that found NOTHING — the case that used to be one bucket called
// `missing_property` and never improved, because a lookup with nothing to
// cache left the site unarmed forever. It has four outcomes now, and telling
// them apart is what says whether the negative cache is working or refusing.
//
// `absent_cacheable` is a PREDICTION: the classifier runs at helper entry,
// before the lookup, so it reports what rt_prop_absent.cpp will decide from
// the same inputs. The one input it cannot see is whether a
// `*CheckMissingMember` refusal will claim this receiver — true for a handful
// of intrinsic singletons (Math, JSON, Object, Atomics, Array.prototype),
// whose absent reads are therefore counted here as cacheable and are not.
//
// It is reached only where the walk above found the key nowhere, so the
// witness half of the install's condition already holds; what is left to ask
// is the seam, the key, and the chain's structure.
const char* classifyAbsent(ObjectHeader* obj, uint32_t keyIndex) {
    if (!rtNegativeIcEnabled()) return "absent_seam_disabled";
    const std::string& keyStr = rtKeyString(keyIndex);
    uint32_t index = 0;
    if (keyStr == "length" || rtKeyAsIndex(keyStr, index)) return "absent_key_refused";
    if (!obj->chainIsCacheable()) return "absent_chain_unprovable";
    return "absent_cacheable";
}

const char* classifyPropGet(Value objVal, uint32_t keyIndex, InlineCache* ic) {
    if (!ic) return "no_ic_slot";
    if (!objVal.isObject()) {
        if (objVal.isNull() || objVal.isUndefined()) return "primitive_null_or_undefined";
        return "primitive_other";
    }
    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    if (hdr->flags == HeapKind::Array) {
        auto* arr = reinterpret_cast<const ArrayHeader*>(hdr);
        if (arr->properties.isObject()) return "array_shadowed_by_side_object";
        if (bronze_tls_block_addr()->array_method_ic_enabled == 0) return "seam_disabled";
        if (!ic->cached_shape) return "ic_uninitialized";
        if (ic->isArrayMethod()) return "array_method_ic_hit_or_other";
        return "shape_mismatch_polymorphic";
    }
    if (hdr->flags == TypedArrayHeader::kFlags) return "kind_typed_array";
    if (hdr->flags == HeapKind::Function) return "kind_function";
    if (hdr->flags == MapHeader::kMapFlags || hdr->flags == MapHeader::kSetFlags) return "kind_map_or_set";
    if (hdr->flags == MapHeader::kWeakMapFlags || hdr->flags == MapHeader::kWeakSetFlags) return "kind_weak_collection";
    if (hdr->flags == ArrayBufferHeader::kFlags) return "kind_array_buffer";
    if (hdr->flags == DataViewHeader::kFlags) return "kind_data_view";
    if (hdr->flags == WeakRefHeader::kFlags) return "kind_weak_ref";
    if (hdr->flags == FinalizationRegistryHeader::kFlags) return "kind_finalization_registry";
    if (hdr->flags == RegExpHeader::kFlags) return "kind_regexp";
    if (hdr->flags == ModuleNamespaceHeader::kFlags) return "kind_module_namespace";
    if (hdr->flags == HeapKind::Proxy) return "kind_proxy";
    if (hdr->flags != HeapKind::Plain) return "kind_other";

    auto* obj = reinterpret_cast<ObjectHeader*>(hdr);
    if (obj->shape && obj->shape->dict) return "receiver_in_dict_mode";

    StringHeader* keyHdr = rtKeyHeader(keyIndex);
    if (keyHdr) {
        PropertyKey pk = PropertyKey::forString(keyHdr);
        ObjectHeader* holder = obj;
        for (uint32_t depth = 0; depth <= ObjectHeader::kMaxPrototypeDepth; ++depth) {
            PropertyInfo info;
            if (holder->shape && holder->shape->lookupProperty(pk, info)) {
                if (info.accessor) return "accessor_getter";
                break;
            }
            ObjectHeader* next = holder->protoAncestor(1);
            if (!next) break;
            holder = next;
        }
    }

    // Which WAY answered, if any. The reasons below separate the three states a
    // multi-way site can be in and that a single-entry one could not tell
    // apart: never touched, still warming with ways to spare, and full — which
    // is the only one of the three that means the site is genuinely wider than
    // the cache and will keep missing.
    auto* site = reinterpret_cast<InlineCacheSite*>(ic);
    const uint32_t limit = rtIcWayLimit();
    ic = site->find(obj->shape, limit);
    if (!ic) {
        bool anyFilled = false;
        bool anyFree = false;
        for (uint32_t i = 0; i < limit && i < BRONZE_ABI_IC_WAYS; ++i) {
            if (site->ways[i].cached_shape) {
                anyFilled = true;
            } else {
                anyFree = true;
            }
        }
        if (!anyFilled) return "ic_uninitialized";
        if (anyFree) return "poly_ic_ways_free";
        return limit > 1 ? "poly_ic_full_rotation" : "shape_mismatch_polymorphic";
    }

    // A way DID name this shape. If it is the negative entry, the only thing
    // that can have sent the read here is a prototype-chain mutation since the
    // fill — the shape compare already matched.
    if (ic->isAbsent()) return "negative_ic_epoch_stale";

    if (ic->realDepth() > 0) {
        if (ic->cached_epoch != protoMutationEpoch()) return "proto_epoch_stale";
        bool crossedDict = false;
        ObjectHeader* holder = obj->cachedProtoHolder(ic->realDepth(), crossedDict);
        if (crossedDict) return "proto_dict_mode";
        if (!holder) return "proto_non_plain_or_null";
        if (keyHdr) {
            PropertyKey pk = PropertyKey::forString(keyHdr);
            PropertyInfo info;
            if (holder->shape && holder->shape->lookupProperty(pk, info)) {
                if (info.accessor) return "accessor_getter";
                return "proto_overflow_or_other";
            }
        }
        return classifyAbsent(obj, keyIndex);
    }

    if (keyHdr) {
        PropertyKey pk = PropertyKey::forString(keyHdr);
        PropertyInfo info;
        if (obj->shape && obj->shape->lookupProperty(pk, info)) {
            if (info.accessor) return "accessor_getter";
            return "depth0_overflow_or_other";
        }
    }
    return classifyAbsent(obj, keyIndex);
}

// Write sites use way 0 and only way 0 (bronze_abi.h): a write's bill is
// transitions rather than shape variety, so `ic` here is the site's first
// entry and the other ways stay zero for the life of the program.
const char* classifyPropSet(Value objVal, uint32_t keyIndex, Value valVal, InlineCache* ic, bool strict) {
    (void)valVal;
    (void)strict;
    if (!ic) return "no_ic_slot";
    if (!objVal.isObject()) return "primitive_receiver";
    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    if (hdr->flags == HeapKind::Array) return "kind_array";
    if (hdr->flags == TypedArrayHeader::kFlags) return "kind_typed_array";
    if (hdr->flags == HeapKind::Function) return "kind_function";
    if (hdr->flags != HeapKind::Plain) return "kind_other";

    auto* obj = reinterpret_cast<ObjectHeader*>(hdr);
    if (obj->shape && obj->shape->dict) return "receiver_in_dict_mode";

    StringHeader* keyHdr = rtKeyHeader(keyIndex);
    if (keyHdr) {
        PropertyKey pk = PropertyKey::forString(keyHdr);
        ObjectHeader* holder = obj;
        for (uint32_t depth = 0; depth <= ObjectHeader::kMaxPrototypeDepth; ++depth) {
            PropertyInfo info;
            if (holder->shape && holder->shape->lookupProperty(pk, info)) {
                if (info.accessor) return "accessor_setter";
                break;
            }
            ObjectHeader* next = holder->protoAncestor(1);
            if (!next) break;
            holder = next;
        }
    }

    if (!ic->cached_shape) return "ic_uninitialized";
    if (ic->isArrayMethod()) return "array_method_sentinel_at_set_site";
    if (ic->cached_shape == obj->shape) {
        if (ic->cached_depth > 0) return "inherited_prop_set";
        if (obj->shape && obj->shape->dict) return "receiver_dict_mode";
        if (keyHdr) {
            PropertyKey pk = PropertyKey::forString(keyHdr);
            PropertyInfo info;
            if (obj->shape && obj->shape->lookupProperty(pk, info) && info.accessor) {
                return "accessor_setter";
            }
        }
        return "depth0_overflow_or_other";
    }

    if (ic->isRealShape() && ic->cached_shape->parent == obj->shape) {
        uint64_t slotWord = *reinterpret_cast<const uint64_t*>(
            reinterpret_cast<const char*>(ic) + BRONZE_ABI_IC_SLOT_OFFSET);
        uint32_t slot = static_cast<uint32_t>(slotWord);
        uint32_t depth = static_cast<uint32_t>(slotWord >> 32);
        if (depth != 0) return "transition_inherited_slot";
        if (slot >= BRONZE_ABI_OBJ_INLINE_SLOTS) {
            if (bronze_tls_block_addr()->inline_overflow_set_enabled == 0) return "seam_disabled";
            if (!obj->overflow.isObject()) return "transition_overflow_alloc_needed";
            uint32_t cap = obj->overflowCapacity();
            if (slot - BRONZE_ABI_OBJ_INLINE_SLOTS >= cap) return "transition_overflow_growth_needed";
        }
        if (ic->cached_shape->slot_index != slot) {
            return "transition_key_mismatch";
        }
        if (!(ic->cached_shape->enumerable && !ic->cached_shape->accessor &&
              ic->cached_shape->writable && ic->cached_shape->configurable)) {
            return "transition_attrs_not_plain";
        }
        if (obj->shape && obj->shape->used_as_prototype) {
            return "transition_receiver_used_as_proto";
        }
        if (ic->cached_epoch != protoMutationEpoch()) {
            return "transition_epoch_stale";
        }
        const std::string& keyStr = rtKeyString(keyIndex);
        if (keyStr == "length" || rtKeyInfo(keyIndex).isElemIndex) {
            return "transition_special_key";
        }
        return "transition_arm_miss_other";
    }

    return "shape_mismatch_polymorphic";
}

const char* classifyDynamicCall(uint64_t calleeBits, uint32_t argc, std::string& calleeDesc) {
    Value calleeVal(calleeBits);
    if (!calleeVal.isObject()) {
        calleeDesc = "(non-object)";
        return "tag_not_object";
    }
    auto* hdr = calleeVal.asObject<HeapObjectHeader>();
    if (hdr->flags != HeapKind::Function) {
        calleeDesc = "(non-function object)";
        return "not_function_kind";
    }
    auto* fn = reinterpret_cast<FunctionHeader*>(hdr);
    std::string nameStr;
    if (fn->name) {
        if (fn->name->getLength() == 0) {
            nameStr = "anonymous";
        } else {
            nameStr = rtUtf8Chars(fn->name);
        }
    } else {
        nameStr = "native/unnamed";
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "fn \"%s\" (arity %u, argc %u)", nameStr.c_str(), fn->arity, argc);
    calleeDesc = buf;

    if (fn->arity > argc) {
        return "under_arity_padding";
    }
    if (bronze_tls_block_addr()->inline_call_enabled == 0) {
        return "seam_disabled";
    }
    return "module_has_new_target_or_direct";
}

struct AutoIcLogInit {
    AutoIcLogInit() {
        initIcLog();
    }
} s_autoIcLogInit;

}  // namespace

void initIcLog() {
    if (s_initialized) return;
    s_initialized = true;
    const char* env = std::getenv("BRONZE_IC_LOG");
    if (env && std::strcmp(env, "1") == 0) {
        g_icLogEnabled = true;
        std::atexit(dumpIcLogReport);
    }
}

void icLogRecordPropGet(uint64_t objBits, uint32_t keyIndex, uint64_t* icEntry) {
    Value objVal(objBits);
    InlineCache* ic = rtAsCache(icEntry);
    const char* reason = classifyPropGet(objVal, keyIndex, ic);
    std::string key = "." + rtKeyString(keyIndex);

    g_propGetStats.totalCount++;
    g_propGetStats.reasonCounts[reason]++;
    auto& ks = g_propGetStats.keyStats[key];
    ks.totalCount++;
    ks.reasonCounts[reason]++;
}

void icLogRecordPropSet(uint64_t objBits, uint32_t keyIndex, uint64_t valBits, uint64_t* icEntry, bool strict) {
    Value objVal(objBits);
    Value valVal(valBits);
    InlineCache* ic = rtAsCache(icEntry);
    const char* reason = classifyPropSet(objVal, keyIndex, valVal, ic, strict);
    std::string key = "." + rtKeyString(keyIndex);

    g_propSetStats.totalCount++;
    g_propSetStats.reasonCounts[reason]++;
    auto& ks = g_propSetStats.keyStats[key];
    ks.totalCount++;
    ks.reasonCounts[reason]++;
}

void icLogRecordElemIcMiss(const char* reason, uint64_t objBits, uint64_t keyBits) {
    g_elemIcStats.totalCount++;
    const char* r = reason ? reason : "(none)";
    g_elemIcStats.reasonCounts[r]++;
    // The receiver's nearest shape keys and the key asked for: a computed site
    // names no key of its own, so the only way to say WHICH read refused is to
    // spell the pair the read was about.
    std::string desc;
    const Value obj(objBits);
    const Value key(keyBits);
    if (obj.isObject() && obj.asObject<HeapObjectHeader>()->flags == BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        int shown = 0;
        for (const Shape* n = obj.asObject<ObjectHeader>()->shape; n && n->parent && shown < 3;
             n = n->parent) {
            if (StringHeader* ks = n->key.string()) {
                if (!desc.empty()) desc += ",";
                desc += rtUtf8Chars(ks);
                ++shown;
            }
        }
    }
    desc = "{" + desc + "}";
    if (key.isString()) {
        desc += "." + rtUtf8Chars(key.asString<StringHeader>());
    } else if (key.isNumber()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "[%g]", key.asNumber());
        desc += buf;
    } else if (key.isBool()) {
        desc += key.asBool() ? "[true]" : "[false]";
    } else {
        desc += "[other]";
    }
    auto& ks = g_elemIcStats.keyStats[desc];
    ks.totalCount++;
    ks.reasonCounts[r]++;
}

void icLogRecordDynamicCall(uint64_t calleeBits, uint64_t thisBits, uint32_t argc, const uint64_t* argvBits) {
    (void)thisBits;
    (void)argvBits;
    std::string calleeDesc;
    const char* reason = classifyDynamicCall(calleeBits, argc, calleeDesc);

    g_dynamicCallStats.totalCount++;
    g_dynamicCallStats.reasonCounts[reason]++;
    auto& ks = g_dynamicCallStats.keyStats[calleeDesc];
    ks.totalCount++;
    ks.reasonCounts[reason]++;
}

static void printSectionReport(const char* sectionTitle, const char* keyColumnTitle, const SectionStats& stats) {
    if (stats.totalCount == 0) return;

    std::fprintf(stderr, "\n--- %s by Reason (Total: %llu) ---\n",
                 sectionTitle, static_cast<unsigned long long>(stats.totalCount));
    std::fprintf(stderr, "%-48s %12s %8s\n", "Miss Reason", "Count", "% Total");
    std::fprintf(stderr, "-----------------------------------------------------------------------\n");

    std::vector<std::pair<std::string, uint64_t>> sortedReasons(
        stats.reasonCounts.begin(), stats.reasonCounts.end());
    std::sort(sortedReasons.begin(), sortedReasons.end(),
              [](const auto& a, const auto& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;
              });

    for (const auto& r : sortedReasons) {
        double pct = 100.0 * r.second / stats.totalCount;
        std::fprintf(stderr, "%-48s %12llu %7.1f%%\n",
                     r.first.c_str(),
                     static_cast<unsigned long long>(r.second),
                     pct);
    }
    std::fprintf(stderr, "-----------------------------------------------------------------------\n");

    std::fprintf(stderr, "\n--- %s by %s (Top 20) ---\n", sectionTitle, keyColumnTitle);
    std::fprintf(stderr, "%-38s %10s %7s  %-20s\n", keyColumnTitle, "Count", "% Tot", "Top Miss Reason");
    std::fprintf(stderr, "-----------------------------------------------------------------------\n");

    std::vector<std::pair<std::string, KeyStats>> sortedKeys(
        stats.keyStats.begin(), stats.keyStats.end());
    std::sort(sortedKeys.begin(), sortedKeys.end(),
              [](const auto& a, const auto& b) {
                  if (a.second.totalCount != b.second.totalCount) {
                      return a.second.totalCount > b.second.totalCount;
                  }
                  return a.first < b.first;
              });

    size_t count = 0;
    for (const auto& [k, ks] : sortedKeys) {
        if (++count > 20) {
            size_t remaining = sortedKeys.size() - 20;
            std::fprintf(stderr, "  ... and %zu more entry(s)\n", remaining);
            break;
        }
        std::string topReason = "(none)";
        uint64_t topReasonCount = 0;
        for (const auto& [r, rc] : ks.reasonCounts) {
            if (rc > topReasonCount || (rc == topReasonCount && r < topReason)) {
                topReasonCount = rc;
                topReason = r;
            }
        }

        double pct = 100.0 * ks.totalCount / stats.totalCount;
        std::string label = k;
        if (label.size() > 38) label = label.substr(0, 35) + "...";
        std::fprintf(stderr, "%-38s %10llu %6.1f%%  %-20s\n",
                     label.c_str(),
                     static_cast<unsigned long long>(ks.totalCount),
                     pct,
                     topReason.c_str());
    }
    std::fprintf(stderr, "-----------------------------------------------------------------------\n");
}

void dumpIcLogReport() {
    if (!g_icLogEnabled) return;

    std::fprintf(stderr, "\n=======================================================================\n");
    std::fprintf(stderr, "=== Bronze Property & Dynamic Call Miss Attribution (BRONZE_IC_LOG=1) ===\n");
    std::fprintf(stderr, "=======================================================================\n");

    printSectionReport("bronze_prop_get Misses", "Key", g_propGetStats);
    printSectionReport("bronze_prop_set Misses", "Key", g_propSetStats);
    printSectionReport("bronze_dynamic_call Misses", "Callee", g_dynamicCallStats);
    printSectionReport("bronze_elem_get Cache Misses", "Key", g_elemIcStats);

    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

}  // namespace bronze::runtime
