#include "runtime/slot_repr.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <atomic>
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

// The eligible names, arena-interned and therefore immortal, sorted by their
// content hash: a lookup is a binary search for the key's hash and a content
// compare of the names that share it. A harvest-fed build pins hundreds of
// names, and a linear scan of them ran on every property a store creates.
//
// LEAKED, like the shape census's site table and for the same reason: the
// report runs from `std::atexit`, and a function-local static constructed after
// the handler was registered is destroyed BEFORE it runs. Every container in
// this file that the report reads is allocated the same way.
// Read on every store that creates a Number-valued property (object.cpp,
// desiredSlotRepr), so readers take no lock: the list is an immutable
// snapshot behind an atomic pointer, and a registration (a module's load)
// publishes a new one under the writers' mutex. Old snapshots are leaked with
// the rest; there is one per registered name.
std::mutex& eligibleNamesMutex() {
    static auto* m = new std::mutex();
    return *m;
}

// A name and the representations it may be born in, as a mask of
// `1 << SlotRepr`.
struct EligibleName {
    uint32_t hash;
    StringHeader* name;
    uint8_t reprs;
};

bool hashBelow(const EligibleName& e, uint32_t hash) noexcept { return e.hash < hash; }
using EligibleNames = std::vector<EligibleName>;
std::atomic<const EligibleNames*> g_eligibleNames{nullptr};

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
    bool stats = false;
    bool census = false;
};

Config& config() {
    static Config c = [] {
        Config out;
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

bool slotReprCensusEnabled() noexcept { return config().census; }

void slotReprResetForTesting() {
    {
        std::lock_guard<std::mutex> lock(eligibleNamesMutex());
        g_eligibleNames.store(nullptr, std::memory_order_release);
    }
    slotReprMutableCounters() = SlotReprCounters{};
    std::lock_guard<std::mutex> lock(censusMutex());
    censusTable().clear();
}

SlotReprCounters& slotReprMutableCounters() noexcept {
    static auto* c = new SlotReprCounters();
    return *c;
}
const SlotReprCounters& slotReprCounters() noexcept { return slotReprMutableCounters(); }

NonMovingArena& slotReprArena() {
    static auto* a = new NonMovingArena();
    return *a;
}

void slotReprRegisterName(StringHeader* name, SlotRepr repr) {
    if (name == nullptr || repr == SlotRepr::Boxed) return;
    const uint8_t bit = static_cast<uint8_t>(1u << static_cast<uint8_t>(repr));
    const uint32_t hash = name->hash();
    std::lock_guard<std::mutex> lock(eligibleNamesMutex());
    const EligibleNames* current = g_eligibleNames.load(std::memory_order_acquire);
    auto* next = current != nullptr ? new EligibleNames(*current) : new EligibleNames();
    auto at = std::lower_bound(next->begin(), next->end(), hash, hashBelow);
    for (auto it = at; it != next->end() && it->hash == hash; ++it) {
        if (!it->name->equals(*name)) continue;
        if ((it->reprs & bit) != 0) {
            delete next;
            return;
        }
        it->reprs |= bit;
        g_eligibleNames.store(next, std::memory_order_release);
        return;
    }
    StringHeader* kept = StringHeader::createFromUTF8InArena(slotReprArena(), rtUtf8Chars(name));
    // Cached now, under the lock: a reader on another thread compares the
    // shared copy and must not be the one to write its hash word.
    kept->hash();
    next->insert(at, EligibleName{hash, kept, bit});
    g_eligibleNames.store(next, std::memory_order_release);
}

uint32_t slotReprEligibleCount() noexcept {
    const EligibleNames* names = g_eligibleNames.load(std::memory_order_acquire);
    return names != nullptr ? static_cast<uint32_t>(names->size()) : 0;
}

bool slotReprEligible(PropertyKey key, SlotRepr repr) noexcept {
    const EligibleNames* names = g_eligibleNames.load(std::memory_order_acquire);
    if (names == nullptr || names->empty() || !key.valid() || key.isSymbol()) return false;
    const StringHeader* s = key.string();
    const uint32_t hash = s->hash();
    const uint8_t bit = static_cast<uint8_t>(1u << static_cast<uint8_t>(repr));
    for (auto it = std::lower_bound(names->begin(), names->end(), hash, hashBelow);
         it != names->end() && it->hash == hash; ++it) {
        if (it->name == s || it->name->equals(*s)) return (it->reprs & bit) != 0;
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
                 "\n=== slot representation ===\n"
                 "  eligible names  : %u\n"
                 "  shape nodes     : %llu double, %llu object, %llu boxed\n"
                 "  refused         : %llu (number store, name not eligible)\n"
                 "  generalizations : %llu store%s over %llu node%s\n"
                 "  helper stores   : %llu into a double slot\n",
                 slotReprEligibleCount(),
                 static_cast<unsigned long long>(c.double_nodes),
                 static_cast<unsigned long long>(c.object_nodes),
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
    // numbers and whose representation is still BOXED is a candidate for a
    // wider pins manifest, and a DOUBLE slot with any non-number store is a policy
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
                 "candidates for a wider manifest\n\n",
                 static_cast<unsigned long long>(stableBoxed),
                 static_cast<unsigned long long>(stableBoxedTraffic));
}

extern "C" {

// Each entry is a module key index; BRONZE_ABI_SLOT_REPR_FIELD_OBJECT set in
// it makes the name eligible for an object slot rather than a double one.
void bronze_register_slot_repr(const uint32_t* fields, uint32_t count, const uint32_t* keyMap) {
    recordHelperCall("bronze_register_slot_repr");
    if (fields == nullptr || keyMap == nullptr || count == 0) return;
    for (uint32_t i = 0; i < count; ++i) {
        const bool object = (fields[i] & BRONZE_ABI_SLOT_REPR_FIELD_OBJECT) != 0;
        slotReprRegisterName(rtKeyHeader(keyMap[fields[i] & ~BRONZE_ABI_SLOT_REPR_FIELD_OBJECT]),
                             object ? SlotRepr::Object : SlotRepr::Double);
    }
}

}  // extern "C"

}  // namespace bronze::runtime
