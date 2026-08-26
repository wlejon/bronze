#include "runtime/slot_repr.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/profile.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/shape_census.h"
#include "runtime/string.h"

namespace bronze::runtime {

namespace {

bool envIsOne(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

// The eligible names, arena-interned and therefore immortal. A vector and a
// linear `matches` scan rather than a hash set: the list is the pinned fields
// of a program's math classes (seventeen, for three.js), it is consulted only
// when a shape NODE IS CREATED, and content-matching an interned key is a
// pointer compare in the common case.
//
// LEAKED, like the shape census's site table and for the same reason: the
// report runs from `std::atexit`, and a function-local static constructed after
// the handler was registered is destroyed BEFORE it runs. Every container in
// this file that the report reads is allocated the same way.
std::vector<StringHeader*>& eligibleNames() {
    static auto* v = new std::vector<StringHeader*>();
    return *v;
}

// Per-(shape, slot) representation stability, for BRONZE_SLOT_REPR_CENSUS.
struct SlotRec {
    uint64_t numberStores = 0;
    uint64_t otherStores = 0;
    uint64_t reads = 0;
    bool isDouble = false;
    std::string key;
};

std::mutex& censusMutex() {
    static auto* m = new std::mutex();
    return *m;
}
std::map<std::pair<const Shape*, uint32_t>, SlotRec>& censusTable() {
    static auto* t = new std::map<std::pair<const Shape*, uint32_t>, SlotRec>();
    return *t;
}

// The state every accessor above reads, resolved once. A function-local static
// rather than a namespace-scope one so that a runtime translation unit whose
// static initialization runs before this one cannot observe it half-built.
struct Config {
    bool enabled = true;
    bool observesUnpinned = false;
    bool stats = false;
    bool census = false;
};

Config& config() {
    static Config c = [] {
        Config out;
        out.enabled = !envIsOne("BRONZE_NO_SLOT_REPR");
        out.observesUnpinned = out.enabled && envIsOne("BRONZE_SLOT_REPR_OBSERVED");
        out.stats = envIsOne("BRONZE_SLOT_REPR_STATS");
        out.census = envIsOne("BRONZE_SLOT_REPR_CENSUS");
        if (out.stats || out.census) std::atexit(slotReprReport);
        // The census has to see the traffic that inline caches would otherwise
        // absorb, and the shape census already owns that suppression — one
        // global every latch site in the runtime consults. Borrowing it is what
        // keeps this mode from needing a second set of hooks in the same eight
        // files.
        if (out.census) g_shapeCensusEnabled = true;
        return out;
    }();
    return c;
}

// Up to three own key names, newest first — the same spelling the shape census
// gives a shape, so the two reports name the same layouts identically.
std::string shapeDesc(const Shape* sh) {
    if (sh == nullptr) return "?";
    std::string keys;
    int shown = 0;
    for (const Shape* n = sh; n != nullptr && n->parent != nullptr && shown < 3; n = n->parent) {
        if (StringHeader* ks = n->key.string()) {
            if (!keys.empty()) keys += ",";
            keys += rtUtf8Chars(ks);
            ++shown;
        }
    }
    return (sh->isDictionary() ? "dict{" : "plain{") + keys + "}";
}

}  // namespace

bool slotReprEnabled() noexcept { return config().enabled; }
bool slotReprObservesUnpinned() noexcept { return config().observesUnpinned; }
bool slotReprCensusEnabled() noexcept { return config().census; }

void slotReprSetEnabledForTesting(bool enabled) noexcept { config().enabled = enabled; }
void slotReprSetObservesUnpinnedForTesting(bool enabled) noexcept {
    config().observesUnpinned = enabled;
}

void slotReprResetForTesting() {
    eligibleNames().clear();
    slotReprMutableCounters() = SlotReprCounters{};
    std::lock_guard<std::mutex> lock(censusMutex());
    censusTable().clear();
}

SlotReprCounters& slotReprMutableCounters() noexcept {
    static auto* c = new SlotReprCounters();
    return *c;
}
const SlotReprCounters& slotReprCounters() noexcept { return slotReprMutableCounters(); }

void slotReprRegisterName(StringHeader* name) {
    if (name == nullptr) return;
    const PropertyKey incoming = PropertyKey::forString(name);
    for (StringHeader* existing : eligibleNames()) {
        if (PropertyKey::forString(existing).matches(incoming)) return;
    }
    eligibleNames().push_back(name);
}

uint32_t slotReprEligibleCount() noexcept {
    return static_cast<uint32_t>(eligibleNames().size());
}

bool slotReprEligible(PropertyKey key) noexcept {
    if (!key.valid() || key.isSymbol()) return false;
    if (config().observesUnpinned) return true;
    for (StringHeader* name : eligibleNames()) {
        if (PropertyKey::forString(name).matches(key)) return true;
    }
    return false;
}

void slotReprCensusNote(const Shape* shape, PropertyKey key, bool hasValue, Value stored) {
    if (!config().census || shape == nullptr || shape->isDictionary() || !key.valid()) return;
    PropertyInfo info;
    if (!shape->lookupProperty(key, info) || info.accessor) return;

    std::lock_guard<std::mutex> lock(censusMutex());
    SlotRec& rec = censusTable()[{shape, info.slot}];
    if (rec.key.empty()) {
        if (StringHeader* s = key.string()) rec.key = rtUtf8Chars(s);
        if (rec.key.empty()) rec.key = "(symbol)";
        rec.isDouble = shape->slotIsDouble(info.slot);
    }
    if (!hasValue) {
        rec.reads++;
        return;
    }
    if (slotReprAcceptsValue(stored)) {
        rec.numberStores++;
    } else {
        rec.otherStores++;
    }
}

void slotReprReport() {
    const SlotReprCounters& c = slotReprCounters();
    std::fprintf(stderr,
                 "\n=== slot representation (stage R1) ===\n"
                 "  seam            : %s%s\n"
                 "  eligible names  : %u\n"
                 "  shape nodes     : %llu double, %llu boxed\n"
                 "  refused         : %llu (number store, name not eligible)\n"
                 "  generalizations : %llu store%s over %llu node%s\n"
                 "  double stores   : %llu\n",
                 slotReprEnabled() ? "on" : "off (BRONZE_NO_SLOT_REPR=1)",
                 slotReprObservesUnpinned() ? " + observed-unpinned" : "",
                 slotReprEligibleCount(),
                 static_cast<unsigned long long>(c.double_nodes),
                 static_cast<unsigned long long>(c.boxed_nodes),
                 static_cast<unsigned long long>(c.refused_ineligible),
                 static_cast<unsigned long long>(c.generalizations),
                 c.generalizations == 1 ? "" : "s",
                 static_cast<unsigned long long>(c.generalized_nodes),
                 c.generalized_nodes == 1 ? "" : "s",
                 static_cast<unsigned long long>(c.double_stores));

    if (!config().census) {
        std::fprintf(stderr,
                     "  (BRONZE_SLOT_REPR_CENSUS=1 adds per-(shape, slot) stability)\n\n");
        return;
    }

    // Per-(shape, slot) stability, worst first: a slot whose stores are all
    // numbers and whose representation is still BOXED is exactly what stage R2
    // is looking for, and a DOUBLE slot with any non-number store is a policy
    // mistake this run caught.
    struct Row {
        std::string shape;
        uint32_t slot;
        SlotRec rec;
    };
    std::vector<Row> rows;
    {
        std::lock_guard<std::mutex> lock(censusMutex());
        rows.reserve(censusTable().size());
        for (const auto& [k, v] : censusTable()) {
            rows.push_back(Row{shapeDesc(k.first), k.second, v});
        }
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        const uint64_t ta = a.rec.numberStores + a.rec.otherStores + a.rec.reads;
        const uint64_t tb = b.rec.numberStores + b.rec.otherStores + b.rec.reads;
        return ta > tb;
    });

    uint64_t stableBoxed = 0;
    uint64_t stableBoxedTraffic = 0;
    std::fprintf(stderr,
                 "\n  per-(shape, slot) representation stability — %zu slots\n"
                 "  %-28s %-4s %-14s %8s %8s %8s  %s\n",
                 rows.size(), "shape", "slot", "key", "num", "other", "reads", "repr");
    size_t shown = 0;
    for (const Row& r : rows) {
        const bool stable = r.rec.otherStores == 0 && r.rec.numberStores > 0;
        if (stable && !r.rec.isDouble) {
            ++stableBoxed;
            stableBoxedTraffic += r.rec.numberStores + r.rec.reads;
        }
        if (shown++ < 40) {
            std::fprintf(stderr, "  %-28s %-4u %-14s %8llu %8llu %8llu  %s%s\n",
                         r.shape.c_str(), r.slot, r.rec.key.c_str(),
                         static_cast<unsigned long long>(r.rec.numberStores),
                         static_cast<unsigned long long>(r.rec.otherStores),
                         static_cast<unsigned long long>(r.rec.reads),
                         r.rec.isDouble ? "double" : "boxed",
                         (r.rec.isDouble && r.rec.otherStores > 0) ? "  <-- VIOLATED" : "");
        }
    }
    if (rows.size() > 40) std::fprintf(stderr, "  ... %zu more\n", rows.size() - 40);
    std::fprintf(stderr,
                 "  boxed slots whose every store was a number: %llu (%llu accesses) — "
                 "stage R2's candidate set\n\n",
                 static_cast<unsigned long long>(stableBoxed),
                 static_cast<unsigned long long>(stableBoxedTraffic));
}

extern "C" {

void bronze_register_slot_repr(const uint32_t* fields, uint32_t count, const uint32_t* keyMap) {
    recordHelperCall("bronze_register_slot_repr");
    if (fields == nullptr || keyMap == nullptr || count == 0) return;
    if (!slotReprEnabled()) return;
    for (uint32_t i = 0; i < count; ++i) {
        slotReprRegisterName(rtKeyHeader(keyMap[fields[i]]));
    }
}

}  // extern "C"

}  // namespace bronze::runtime
