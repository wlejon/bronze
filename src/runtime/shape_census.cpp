
#include "runtime/shape_census.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "runtime/object.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/slot_repr.h"
#include "runtime/string.h"
#include "runtime/symbolize.h"
#include "runtime/value.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace bronze::runtime {

bool g_shapeCensusEnabled = false;

namespace {

// --- receiver identity ------------------------------------------------------
//
// A PLAIN object's identity is its Shape* — immortal, non-moving arena memory,
// safe to hold in the census map for the life of the process. Every other
// receiver gets a pseudo-shape: a small ODD sentinel (a real shape is 8-byte
// aligned arena memory, so no collision is possible). The census's poly
// degree then means "distinct layouts" for plain receivers and "distinct
// receiver kinds" for the rest, which is the same question the inline caches
// ask.
const void* pseudoShape(uint32_t kindTag) {
    return reinterpret_cast<const void*>(static_cast<uintptr_t>((kindTag << 1) | 1u));
}

const void* receiverIdentity(Value v, bool& isPlainShape) {
    isPlainShape = false;
    if (v.isObject()) {
        HeapObjectHeader* hdr = v.asObject<HeapObjectHeader>();
        if (hdr->flags == HeapKind::Plain) {
            const Shape* sh = reinterpret_cast<ObjectHeader*>(hdr)->shape;
            if (sh != nullptr) {
                isPlainShape = true;
                return sh;
            }
            return pseudoShape(100);
        }
        return pseudoShape(static_cast<uint32_t>(hdr->flags));
    }
    if (v.isString()) return pseudoShape(200);
    if (v.isNumber()) return pseudoShape(201);
    if (v.isBool()) return pseudoShape(202);
    if (v.isNull()) return pseudoShape(203);
    if (v.isUndefined()) return pseudoShape(204);
    return pseudoShape(205);
}

std::string identityDesc(const void* id) {
    const uintptr_t bits = reinterpret_cast<uintptr_t>(id);
    if (bits & 1u) {
        const uint32_t tag = static_cast<uint32_t>(bits >> 1);
        switch (tag) {
            case 100: return "plain(no-shape)";
            case 200: return "string";
            case 201: return "number";
            case 202: return "boolean";
            case 203: return "null";
            case 204: return "undefined";
            case 205: return "primitive";
            default: break;
        }
        switch (static_cast<uint16_t>(tag)) {
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
            default: return "kind#" + std::to_string(tag);
        }
    }
    // A real shape: its nearest three own keys, newest first — the same
    // renderer BRONZE_PROFILE's element report uses, so the two reports name
    // one shape one way. Reads only immortal arena memory.
    const Shape* sh = static_cast<const Shape*>(id);
    std::string keys;
    int shown = 0;
    for (const Shape* n = sh; n != nullptr && n->parent != nullptr && shown < 3; n = n->parent) {
        if (StringHeader* ks = n->key.string()) {
            if (!keys.empty()) keys += ",";
            keys += rtUtf8Chars(ks);
            ++shown;
        }
    }
    if (sh != nullptr && sh->isDictionary()) return "dict{" + keys + "}";
    return "plain{" + keys + "}";
}

// --- the census store -------------------------------------------------------

struct SiteRec {
    CensusKind kind{CensusKind::PropGet};
    uint32_t keyIndex = 0xFFFFFFFFu;
    const void* siteId = nullptr;
    const void* retaddr = nullptr;
    const void* retaddr2 = nullptr;  // non-null when a second call PC reached this site
    uint64_t hits = 0;
    std::unordered_map<const void*, uint64_t> shapeCounts;
    // Computed-key sites: what the keys were.
    std::unordered_map<std::string, uint64_t> keyCounts;  // capped; overflow under "(other)"
    uint64_t keyNumIndex = 0;
    uint64_t keyNumOther = 0;
    uint64_t keySymbol = 0;
    // Value-tag traffic: reads classify their RESULT, writes the STORED value.
    uint64_t numberVals = 0;
    uint64_t undefinedVals = 0;
    uint64_t boolVals = 0;
    uint64_t objectVals = 0;
    uint64_t stringVals = 0;
    uint64_t otherVals = 0;
    // Writes whose key was not on the receiver's shape: this store transitions
    // (or dictionary-adds) — allocation-shaped traffic, not slot-update.
    uint64_t transitions = 0;
};

std::mutex g_mu;
std::unordered_map<const void*, SiteRec>* g_sites = nullptr;  // leaked on purpose
thread_local int t_nestedDepth = 0;
bool s_initialized = false;

void dumpShapeCensus();

void initShapeCensus() {
    if (s_initialized) return;
    s_initialized = true;
    const char* env = std::getenv("BRONZE_SHAPE_CENSUS");
    if (env != nullptr && std::strcmp(env, "1") == 0) {
        g_shapeCensusEnabled = true;
        g_sites = new std::unordered_map<const void*, SiteRec>();
        g_sites->reserve(1 << 14);
        std::atexit(dumpShapeCensus);
    }
}

struct AutoCensusInit {
    AutoCensusInit() { initShapeCensus(); }
} s_autoCensusInit;

const char* kindName(CensusKind k) {
    switch (k) {
        case CensusKind::PropGet: return "get";
        case CensusKind::PropSet: return "set";
        case CensusKind::ElemGet: return "elem_get";
        case CensusKind::ElemSet: return "elem_set";
        case CensusKind::MethodGet: return "method_get";
        case CensusKind::SuperGet: return "super_get";
        case CensusKind::SuperSet: return "super_set";
    }
    return "?";
}

void classifyValue(SiteRec& r, uint64_t bits) {
    const Value v(bits);
    if (v.isNumber() || v.tag() == static_cast<uint16_t>(Tag::Int32)) {
        r.numberVals++;
    } else if (v.isUndefined()) {
        r.undefinedVals++;
    } else if (v.isBool()) {
        r.boolVals++;
    } else if (v.isObject()) {
        r.objectVals++;
    } else if (v.isString()) {
        r.stringVals++;
    } else {
        r.otherVals++;
    }
}

}  // namespace

CensusToken censusRecordAccess(CensusKind kind, uint64_t objBits, uint32_t keyIndex,
                               uint64_t keyBits, const void* siteId, const void* retaddr,
                               bool hasValue, uint64_t valBits) {
    if (!g_shapeCensusEnabled || g_sites == nullptr) return nullptr;
    if (t_nestedDepth > 0) return nullptr;

    const Value obj(objBits);
    bool isPlainShape = false;
    const void* identity = receiverIdentity(obj, isPlainShape);

    // Transition detection for writes, BEFORE the store runs: is the key
    // already on the receiver's shape? Reads only arena memory.
    bool transitioned = false;
    if ((kind == CensusKind::PropSet || kind == CensusKind::SuperSet) && isPlainShape &&
        keyIndex != 0xFFFFFFFFu) {
        const Shape* sh = static_cast<const Shape*>(identity);
        if (!sh->isDictionary()) {
            if (StringHeader* keyHdr = rtKeyHeader(keyIndex)) {
                PropertyInfo info;
                transitioned = !sh->lookupProperty(PropertyKey::forString(keyHdr), info);
            }
        }
    }

    // Per-(shape, slot) REPRESENTATION stability, under BRONZE_SLOT_REPR_CENSUS
    // (runtime/slot_repr.h). It rides this recording point rather than owning
    // one of its own because the two want the same thing at the same instant —
    // the receiver's shape before the store runs — and because the latch
    // suppression that makes inline-cache hit traffic visible is already here.
    if (isPlainShape && keyIndex != 0xFFFFFFFFu && slotReprCensusEnabled()) {
        if (StringHeader* keyHdr = rtKeyHeader(keyIndex)) {
            slotReprCensusNote(static_cast<const Shape*>(identity), PropertyKey::forString(keyHdr),
                               hasValue, Value(valBits));
        }
    }

    const void* mapKey = siteId != nullptr ? siteId : retaddr;
    if (mapKey == nullptr) return nullptr;

    std::lock_guard<std::mutex> lock(g_mu);
    SiteRec& r = (*g_sites)[mapKey];
    if (r.hits == 0) {
        r.kind = kind;
        r.keyIndex = keyIndex;
        r.siteId = siteId;
        r.retaddr = retaddr;
    } else if (retaddr != r.retaddr && r.retaddr2 == nullptr && retaddr != nullptr) {
        r.retaddr2 = retaddr;
    }
    r.hits++;
    r.shapeCounts[identity]++;
    if (transitioned) r.transitions++;

    if (keyIndex == 0xFFFFFFFFu) {
        // Computed key: classify it the way the elem helper's ladder does.
        const Value key(keyBits);
        if (key.isNumber()) {
            const double d = key.asNumber();
            const uint32_t u = static_cast<uint32_t>(d);
            const bool isIndex = d >= 0.0 && d <= 4294967294.0 && static_cast<double>(u) == d;
            if (isIndex) {
                r.keyNumIndex++;
            } else {
                r.keyNumOther++;
            }
        } else if (key.isSymbol()) {
            r.keySymbol++;
        } else if (key.isString()) {
            StringHeader* s = key.asString<StringHeader>();
            if (s != nullptr && s->getLength() > 0 && s->getLength() <= 48) {
                if (r.keyCounts.size() < 16 || r.keyCounts.count(rtUtf8Chars(s)) != 0) {
                    r.keyCounts[rtUtf8Chars(s)]++;
                } else {
                    r.keyCounts["(other)"]++;
                }
            } else {
                r.keyCounts["(other)"]++;
            }
        }
    }

    if (hasValue) classifyValue(r, valBits);
    return &r;
}

void censusRecordResult(CensusToken tok, uint64_t resultBits) {
    if (tok == nullptr) return;
    std::lock_guard<std::mutex> lock(g_mu);
    classifyValue(*static_cast<SiteRec*>(tok), resultBits);
}

void censusEnterNested() { t_nestedDepth++; }
void censusLeaveNested() { t_nestedDepth--; }
bool censusInNested() { return t_nestedDepth > 0; }

namespace {

// --- dump -------------------------------------------------------------------

std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char b[8];
            std::snprintf(b, sizeof(b), "\\u%04x", c);
            o += b;
        } else {
            o += c;
        }
    }
    return o;
}

struct FnAgg {
    std::string module;
    uint64_t sites = 0;
    uint64_t obs = 0;
    uint64_t monoObs = 0;
    uint64_t numberVals = 0;
    uint64_t transitions = 0;
};

void dumpShapeCensus() {
    if (!g_shapeCensusEnabled || g_sites == nullptr) return;
    // The census owns no lock discipline at exit: the program is done, the JS
    // thread is this thread, and the map is read-only from here.
    auto& sites = *g_sites;
    if (sites.empty()) return;

    // Monomorphic-in-practice: the site's dominant receiver identity covers
    // >= 99% of its observations (docs/shape-census.md pins the threshold).
    constexpr double kMonoShare = 0.99;

    struct Row {
        const SiteRec* r;
        const void* mapKey;
        uint64_t topCount;
        size_t degree;
        std::string fn;
        std::string module;
    };
    std::vector<Row> rows;
    rows.reserve(sites.size());
    uint64_t totalObs = 0;
    for (auto& [mapKey, r] : sites) {
        Row row;
        row.r = &r;
        row.mapKey = mapKey;
        row.topCount = 0;
        for (const auto& [sh, c] : r.shapeCounts) row.topCount = std::max(row.topCount, c);
        row.degree = r.shapeCounts.size();
        SymbolizedPc sp;
        symbolizePc(reinterpret_cast<uint64_t>(r.retaddr), sp);
        row.fn = sp.resolved ? sp.name : "(unresolved)";
        row.module = sp.module;
        totalObs += r.hits;
        rows.push_back(std::move(row));
    }

    // Per-function aggregation, on the symbolized names — the same names the
    // sampler emits, which is what lets an analysis join the two reports.
    std::unordered_map<std::string, FnAgg> fns;
    for (const Row& row : rows) {
        FnAgg& a = fns[row.fn];
        a.module = row.module;
        a.sites++;
        a.obs += row.r->hits;
        const double share =
            row.r->hits ? static_cast<double>(row.topCount) / static_cast<double>(row.r->hits)
                        : 0.0;
        if (share >= kMonoShare) a.monoObs += row.r->hits;
        a.numberVals += row.r->numberVals;
        a.transitions += row.r->transitions;
    }

    // Histogram by poly degree, sites and observations.
    auto degreeBucket = [](size_t d) -> int {
        if (d <= 4) return static_cast<int>(d);  // 1..4 exact
        if (d <= 8) return 5;                    // "5-8"
        return 6;                                // ">8"
    };
    uint64_t histSites[7] = {0};
    uint64_t histObs[7] = {0};
    uint64_t monoObsTotal = 0;
    for (const Row& row : rows) {
        const int b = degreeBucket(row.degree);
        histSites[b]++;
        histObs[b] += row.r->hits;
        const double share =
            row.r->hits ? static_cast<double>(row.topCount) / static_cast<double>(row.r->hits)
                        : 0.0;
        if (share >= kMonoShare) monoObsTotal += row.r->hits;
    }

    // --- human summary (stderr) --------------------------------------------
    std::fprintf(stderr, "\n=== Bronze Shape Census (BRONZE_SHAPE_CENSUS=1) ===\n");
    std::fprintf(stderr, "sites: %zu, observations: %llu\n", sites.size(),
                 static_cast<unsigned long long>(totalObs));
    static const char* kBucketNames[7] = {"0", "1", "2", "3", "4", "5-8", ">8"};
    std::fprintf(stderr, "poly degree     sites        observations\n");
    for (int b = 1; b <= 6; ++b) {
        std::fprintf(stderr, "  %-11s %8llu %19llu\n", kBucketNames[b],
                     static_cast<unsigned long long>(histSites[b]),
                     static_cast<unsigned long long>(histObs[b]));
    }
    std::fprintf(stderr,
                 "monomorphic-in-practice (top shape >= %.0f%%): %llu of %llu observations "
                 "(%.2f%%)\n",
                 kMonoShare * 100.0, static_cast<unsigned long long>(monoObsTotal),
                 static_cast<unsigned long long>(totalObs),
                 totalObs ? 100.0 * static_cast<double>(monoObsTotal) /
                                static_cast<double>(totalObs)
                          : 0.0);

    std::vector<std::pair<std::string, const FnAgg*>> fnRows;
    fnRows.reserve(fns.size());
    for (auto& [name, a] : fns) fnRows.emplace_back(name, &a);
    std::sort(fnRows.begin(), fnRows.end(), [](const auto& a, const auto& b) {
        return a.second->obs > b.second->obs;
    });
    std::fprintf(stderr, "\ntop functions by site observations:\n");
    std::fprintf(stderr, "%-56s %12s %7s %8s %9s\n", "Function", "Obs", "Sites", "MonoCov",
                 "NumVal%");
    for (size_t i = 0; i < fnRows.size() && i < 25; ++i) {
        const FnAgg& a = *fnRows[i].second;
        std::fprintf(stderr, "%-56.56s %12llu %7llu %7.1f%% %8.1f%%\n",
                     fnRows[i].first.c_str(), static_cast<unsigned long long>(a.obs),
                     static_cast<unsigned long long>(a.sites),
                     a.obs ? 100.0 * static_cast<double>(a.monoObs) / static_cast<double>(a.obs)
                           : 0.0,
                     a.obs ? 100.0 * static_cast<double>(a.numberVals) /
                                 static_cast<double>(a.obs)
                           : 0.0);
    }
    std::fflush(stderr);

    // --- machine artifact (JSON, schema v0) --------------------------------
    const char* outPath = std::getenv("BRONZE_SHAPE_CENSUS_OUT");
    if (outPath == nullptr || outPath[0] == 0) outPath = "bronze_shape_census.json";
    FILE* f = std::fopen(outPath, "wb");
    if (f == nullptr) return;
    std::fprintf(f,
                 "{\n\"version\":\"bronze-shape-census-v0\",\n\"mono_threshold\":%.2f,\n"
                 "\"total_observations\":%llu,\n\"total_sites\":%zu,\n"
                 "\"mono_observations\":%llu,\n",
                 kMonoShare, static_cast<unsigned long long>(totalObs), sites.size(),
                 static_cast<unsigned long long>(monoObsTotal));
    std::fprintf(f, "\"poly_histogram\":{");
    for (int b = 1; b <= 6; ++b) {
        std::fprintf(f, "%s\"%s\":{\"sites\":%llu,\"observations\":%llu}", b > 1 ? "," : "",
                     kBucketNames[b], static_cast<unsigned long long>(histSites[b]),
                     static_cast<unsigned long long>(histObs[b]));
    }
    std::fprintf(f, "},\n\"sites\":[\n");

    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.r->hits > b.r->hits; });
    bool first = true;
    for (const Row& row : rows) {
        const SiteRec& r = *row.r;
        std::string keyName;
        if (r.keyIndex != 0xFFFFFFFFu) keyName = rtKeyString(r.keyIndex);
        std::fprintf(
            f,
            "%s{\"id\":\"%p\",\"kind\":\"%s\",\"key\":\"%s\",\"fn\":\"%s\",\"module\":\"%s\","
            "\"hits\":%llu,\"poly_degree\":%zu,\"top_share\":%.4f,\"mono\":%s,"
            "\"number_vals\":%llu,\"undefined_vals\":%llu,\"bool_vals\":%llu,"
            "\"object_vals\":%llu,\"string_vals\":%llu,\"other_vals\":%llu,"
            "\"transitions\":%llu,\"key_num_index\":%llu,\"key_num_other\":%llu,"
            "\"key_symbol\":%llu,\"shapes\":[",
            first ? "" : ",\n", row.mapKey, kindName(r.kind), jsonEscape(keyName).c_str(),
            jsonEscape(row.fn).c_str(), jsonEscape(row.module).c_str(),
            static_cast<unsigned long long>(r.hits), row.degree,
            r.hits ? static_cast<double>(row.topCount) / static_cast<double>(r.hits) : 0.0,
            (r.hits && static_cast<double>(row.topCount) / static_cast<double>(r.hits) >=
                           kMonoShare)
                ? "true"
                : "false",
            static_cast<unsigned long long>(r.numberVals),
            static_cast<unsigned long long>(r.undefinedVals),
            static_cast<unsigned long long>(r.boolVals),
            static_cast<unsigned long long>(r.objectVals),
            static_cast<unsigned long long>(r.stringVals),
            static_cast<unsigned long long>(r.otherVals),
            static_cast<unsigned long long>(r.transitions),
            static_cast<unsigned long long>(r.keyNumIndex),
            static_cast<unsigned long long>(r.keyNumOther),
            static_cast<unsigned long long>(r.keySymbol));
        first = false;
        // Shapes, largest first, capped at 16 in the artifact.
        std::vector<std::pair<const void*, uint64_t>> shapeRows(r.shapeCounts.begin(),
                                                                r.shapeCounts.end());
        std::sort(shapeRows.begin(), shapeRows.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        for (size_t s = 0; s < shapeRows.size() && s < 16; ++s) {
            std::fprintf(f, "%s{\"shape\":\"%p\",\"count\":%llu,\"desc\":\"%s\"}",
                         s > 0 ? "," : "", shapeRows[s].first,
                         static_cast<unsigned long long>(shapeRows[s].second),
                         jsonEscape(identityDesc(shapeRows[s].first)).c_str());
        }
        std::fprintf(f, "]");
        if (!r.keyCounts.empty()) {
            std::vector<std::pair<std::string, uint64_t>> ks(r.keyCounts.begin(),
                                                             r.keyCounts.end());
            std::sort(ks.begin(), ks.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            std::fprintf(f, ",\"computed_keys\":[");
            for (size_t s = 0; s < ks.size(); ++s) {
                std::fprintf(f, "%s{\"key\":\"%s\",\"count\":%llu}", s > 0 ? "," : "",
                             jsonEscape(ks[s].first).c_str(),
                             static_cast<unsigned long long>(ks[s].second));
            }
            std::fprintf(f, "]");
        }
        std::fprintf(f, "}");
    }

    std::fprintf(f, "\n],\n\"functions\":[\n");
    first = true;
    for (const auto& [name, aggPtr] : fnRows) {
        const FnAgg& a = *aggPtr;
        std::fprintf(f,
                     "%s{\"name\":\"%s\",\"module\":\"%s\",\"sites\":%llu,\"observations\":"
                     "%llu,\"mono_observations\":%llu,\"coverage\":%.4f,"
                     "\"number_vals\":%llu,\"transitions\":%llu}",
                     first ? "" : ",\n", jsonEscape(name).c_str(),
                     jsonEscape(a.module).c_str(), static_cast<unsigned long long>(a.sites),
                     static_cast<unsigned long long>(a.obs),
                     static_cast<unsigned long long>(a.monoObs),
                     a.obs ? static_cast<double>(a.monoObs) / static_cast<double>(a.obs) : 0.0,
                     static_cast<unsigned long long>(a.numberVals),
                     static_cast<unsigned long long>(a.transitions));
        first = false;
    }
    std::fprintf(f, "\n]}\n");
    std::fclose(f);
    std::fprintf(stderr, "shape census: wrote %s\n", outPath);
}

}  // namespace

}  // namespace bronze::runtime
