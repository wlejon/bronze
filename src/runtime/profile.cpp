// getenv, as heap.cpp: the CRT-deprecation opt-out, not a blanket C4996
// disable that would also swallow real deprecated-API uses.
#define _CRT_SECURE_NO_WARNINGS

#include "runtime/profile.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/fn.h"
#include "runtime/object.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

bool g_profileEnabled = false;

namespace {

struct HelperProfile {
    std::string name;
    uint64_t totalCount = 0;
    std::unordered_map<std::string, uint64_t> subSites;
};

static std::unordered_map<std::string, HelperProfile> g_helpers;
static bool s_initialized = false;

// Installed by a layer above the runtime (embed) so it can tell ITS unnamed
// callees apart; null until one is.
static ProfileCalleeNamer s_calleeNamer = nullptr;

// code pointer -> "Owner.member", filled at installation by the native
// factory. Never read on a hot path — only when a callee turns up nameless.
static std::unordered_map<const void*, std::string>& nativeNames() {
    static std::unordered_map<const void*, std::string> m;
    return m;
}

struct AutoProfileInit {
    AutoProfileInit() {
        initProfile();
    }
} s_autoProfileInit;

}  // namespace

void initProfile() {
    if (s_initialized) return;
    s_initialized = true;
    const char* env = std::getenv("BRONZE_PROFILE");
    if (env && std::strcmp(env, "1") == 0) {
        g_profileEnabled = true;
        std::atexit(dumpProfileReport);
    }
}

void profileRecordHelper(const char* helperName) {
    if (!helperName) return;
    auto& h = g_helpers[helperName];
    h.name = helperName;
    h.totalCount++;
}

void profileRecordProp(const char* helperName, uint32_t keyIndex, const void* icSite) {
    if (!helperName) return;
    auto& h = g_helpers[helperName];
    h.name = helperName;
    h.totalCount++;

    std::string key = "." + rtKeyString(keyIndex);
    if (icSite) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), " (site %p)", icSite);
        key += buf;
    }
    h.subSites[key]++;
}

namespace {

// What a receiver IS, for the element report. The helper's own dispatch reads
// exactly this word, so the bucket names name the arms the helper takes.
const char* receiverKindName(Value v) {
    if (!v.isObject()) {
        if (v.isString()) return "string";
        if (v.isNumber()) return "number";
        if (v.isNull()) return "null";
        if (v.isUndefined()) return "undefined";
        return "primitive";
    }
    switch (v.asObject<HeapObjectHeader>()->flags) {
        case HeapKind::Plain: return "plain";
        case HeapKind::Array: return "array";
        case HeapKind::Function: return "function";
        case HeapKind::TypedArray: return "typedarray";
        case HeapKind::ArrayBuffer: return "arraybuffer";
        case HeapKind::DataView: return "dataview";
        case HeapKind::Map: return "map";
        case HeapKind::Set: return "set";
        case HeapKind::Iterator: return "iterator";
        case HeapKind::RegExp: return "regexp";
        case HeapKind::ModuleNamespace: return "namespace";
        case HeapKind::Proxy: return "proxy";
        default: return "other-object";
    }
}

// What the KEY is, split the way the helper's ladder splits it: a number that
// is a clean element index is a different question from one that is not, and a
// string key is the case that funnels all the way to propGetByName.
const char* keyKindName(Value k) {
    if (k.isNumber()) {
        const double d = k.asNumber();
        const uint32_t u = static_cast<uint32_t>(d);
        const bool index = d >= 0.0 && d <= 4294967294.0 && static_cast<double>(u) == d;
        return index ? "num-index" : "num-nonindex";
    }
    if (k.isString()) return "string";
    if (k.isSymbol()) return "symbol";
    if (k.isObject()) return "object";
    if (k.isBool()) return "boolean";
    if (k.isNull()) return "null";
    if (k.isUndefined()) return "undefined";
    return "other";
}

}  // namespace

void profileRecordElem(const char* helperName, uint64_t objBits, uint64_t idxBits) {
    if (!helperName) return;
    auto& h = g_helpers[helperName];
    h.name = helperName;
    h.totalCount++;

    const Value obj(objBits);
    const Value key(idxBits);
    std::string bucket = receiverKindName(obj);
    // A plain receiver is the bucket the bill cannot act on without knowing
    // WHICH object it is: `plain[num-index]` names an arm, not a design. The
    // shape's own keys, nearest three, name the object the way a reader of the
    // library would. Walking the parent chain reads only immortal arena
    // memory, and nothing here allocates on the JS heap.
    if (obj.isObject() && obj.asObject<HeapObjectHeader>()->flags == HeapKind::Plain) {
        const Shape* sh = obj.asObject<ObjectHeader>()->shape;
        std::string keys;
        int shown = 0;
        for (const Shape* n = sh; n && n->parent && shown < 3; n = n->parent) {
            if (StringHeader* ks = n->key.string()) {
                if (!keys.empty()) keys += ",";
                keys += rtUtf8Chars(ks);
                ++shown;
            }
        }
        if (!keys.empty()) bucket += "{" + keys + "}";
    }
    bucket += "[" + std::string(keyKindName(key)) + "]";
    // A string key is the bucket that funnels to the name path, and WHICH name
    // is the whole design question — so those get spelled out. Capped: a site
    // that computes fresh keys must not turn the report into a heap dump.
    if (key.isString()) {
        StringHeader* s = key.asString<StringHeader>();
        if (s && s->getLength() > 0 && s->getLength() <= 32 && h.subSites.size() < 4096) {
            bucket += " ." + rtUtf8Chars(s);
        }
    }
    h.subSites[bucket]++;
}

void profileSetCalleeNamer(ProfileCalleeNamer namer) {
    s_calleeNamer = namer;
}

void profileNameNative(const void* code, std::string_view owner, std::string_view member) {
    if (!g_profileEnabled || !code) return;
    auto& slot = nativeNames()[code];
    if (!slot.empty()) return;  // first installation wins; aliases are noise
    slot.assign(owner);
    if (!owner.empty() && !member.empty()) slot += ".";
    slot.append(member);
}

void profileRecordCall(const char* helperName, uint64_t calleeBits) {
    if (!helperName) return;
    auto& h = g_helpers[helperName];
    h.name = helperName;
    h.totalCount++;

    std::string calleeName = "(unknown)";
    Value v(calleeBits);
    if (v.isObject()) {
        HeapObjectHeader* hdr = v.asObject<HeapObjectHeader>();
        if (hdr && hdr->flags == HeapKind::Function) {
            auto* fn = reinterpret_cast<FunctionHeader*>(hdr);
            if (fn->name) {
                if (fn->name->getLength() == 0) {
                    calleeName = "fn (anonymous)";
                } else {
                    calleeName = "fn \"" + rtUtf8Chars(fn->name) + "\"";
                }
            } else {
                // No `name` slot means native — the runtime's own builtins and
                // every host function answer alike here. Three ways to tell
                // them apart, cheapest first.
                void* code = reinterpret_cast<void*>(fn->code);
                char buf[160];
                if (s_calleeNamer && s_calleeNamer(calleeBits, code, buf, sizeof(buf))) {
                    calleeName = buf;
                } else if (auto it = nativeNames().find(code); it != nativeNames().end()) {
                    calleeName = "fn " + it->second + " (native)";
                } else {
                    std::snprintf(buf, sizeof(buf), "fn (native @%p)", code);
                    calleeName = buf;
                }
            }
        } else if (hdr) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "(non-function kind %u)",
                          static_cast<unsigned>(hdr->flags));
            calleeName = buf;
        }
    }
    h.subSites[calleeName]++;
}

void dumpProfileReport() {
    if (!g_profileEnabled) return;

    uint64_t grandTotal = 0;
    for (const auto& [name, h] : g_helpers) {
        grandTotal += h.totalCount;
    }
    if (grandTotal == 0) return;

    std::vector<HelperProfile> sortedHelpers;
    sortedHelpers.reserve(g_helpers.size());
    for (auto& [name, h] : g_helpers) {
        sortedHelpers.push_back(h);
    }

    std::sort(sortedHelpers.begin(), sortedHelpers.end(),
              [](const HelperProfile& a, const HelperProfile& b) {
                  if (a.totalCount != b.totalCount) {
                      return a.totalCount > b.totalCount;
                  }
                  return a.name < b.name;
              });

    std::fprintf(stderr, "\n=== Bronze Runtime Profile (BRONZE_PROFILE=1) ===\n");
    std::fprintf(stderr, "Total Dynamic ABI Helper Invocations: %llu\n\n",
                 static_cast<unsigned long long>(grandTotal));
    std::fprintf(stderr, "%-48s %12s %8s\n", "Helper / Site", "Count", "% Total");
    std::fprintf(stderr, "-----------------------------------------------------------------------\n");

    for (const auto& h : sortedHelpers) {
        double pct = grandTotal > 0 ? (100.0 * h.totalCount / grandTotal) : 0.0;
        std::fprintf(stderr, "%-48s %12llu %7.1f%%\n",
                     h.name.c_str(),
                     static_cast<unsigned long long>(h.totalCount),
                     pct);

        if (!h.subSites.empty()) {
            std::vector<std::pair<std::string, uint64_t>> sortedSites(
                h.subSites.begin(), h.subSites.end());
            std::sort(sortedSites.begin(), sortedSites.end(),
                      [](const auto& a, const auto& b) {
                          if (a.second != b.second) return a.second > b.second;
                          return a.first < b.first;
                      });

            // Show top sub-sites. Twenty-five rather than ten because the
            // element buckets carry a key name each: a bill that has to name
            // WHICH computed key is hot cannot be read at ten rows.
            const size_t kMaxSites = 25;
            size_t count = 0;
            for (const auto& site : sortedSites) {
                if (++count > kMaxSites) {
                    size_t remaining = sortedSites.size() - kMaxSites;
                    std::fprintf(stderr, "    ... and %zu more site(s)\n", remaining);
                    break;
                }
                double sitePct = grandTotal > 0 ? (100.0 * site.second / grandTotal) : 0.0;
                std::string label = "  " + site.first;
                if (label.size() > 62) label = label.substr(0, 59) + "...";
                std::fprintf(stderr, "%-48s %12llu %7.1f%%\n",
                             label.c_str(),
                             static_cast<unsigned long long>(site.second),
                             sitePct);
            }
        }
    }
    std::fprintf(stderr, "-----------------------------------------------------------------------\n\n");
    std::fflush(stderr);
}

}  // namespace bronze::runtime
