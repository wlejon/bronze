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

// The state every accessor above reads, resolved once. A function-local static
// rather than a namespace-scope one so that a runtime translation unit whose
// static initialization runs before this one cannot observe it half-built.
struct Config {
    bool stats = false;
};

Config& config() {
    static Config c = [] {
        Config out;
        out.stats = envIsOne("BRONZE_SLOT_REPR_STATS");
        if (out.stats) std::atexit(slotReprReport);
        return out;
    }();
    return c;
}

}  // namespace

void slotReprResetForTesting() {
    {
        std::lock_guard<std::mutex> lock(eligibleNamesMutex());
        g_eligibleNames.store(nullptr, std::memory_order_release);
    }
    slotReprMutableCounters() = SlotReprCounters{};
}

SlotReprCounters& slotReprMutableCounters() noexcept {
    // Every counter is bumped on a cold path (a node made, a slot
    // generalized), which is where the report is armed.
    static auto* c = (static_cast<void>(config()), new SlotReprCounters());
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

void slotReprReport() {
    const SlotReprCounters& c = slotReprCounters();
    std::fprintf(stderr,
                 "\n=== slot representation ===\n"
                 "  eligible names  : %u\n"
                 "  shape nodes     : %llu double, %llu object, %llu boxed\n"
                 "  refused         : %llu (number store, name not eligible)\n"
                 "  generalizations : %llu store%s over %llu node%s\n\n",
                 slotReprEligibleCount(),
                 static_cast<unsigned long long>(c.double_nodes),
                 static_cast<unsigned long long>(c.object_nodes),
                 static_cast<unsigned long long>(c.boxed_nodes),
                 static_cast<unsigned long long>(c.refused_ineligible),
                 static_cast<unsigned long long>(c.generalizations),
                 c.generalizations == 1 ? "" : "s",
                 static_cast<unsigned long long>(c.generalized_nodes),
                 c.generalized_nodes == 1 ? "" : "s");
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
