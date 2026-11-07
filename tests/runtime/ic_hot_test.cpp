// The packed way-0 rows (bronze_abi_layout.h, "the HOT array";
// runtime/ic_hot.cpp) at the level JS cannot see them.
//
// Generated code's way-0 guards compare a site's row, not its way 0, so a row
// that lags its site is a guard that misses forever (slow) or, worse, hits on
// an entry the site no longer holds (wrong). The oracle cases pin the answers
// (ic_hot_shape_churn and friends); this file asks the rows themselves: does
// every fill of a registered site's way 0 land in its row, does nothing else,
// and does a range the runtime was told to forget stay forgotten.

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/ic_hot.h"
#include "runtime/object.h"
#include "runtime/shape.h"
#include "runtime/string.h"

using namespace bronze;

namespace {

constexpr size_t kSites = 5;

// A module's table and rows as the backend lays them out, both zero.
struct Tables {
    std::vector<InlineCacheSite> sites = std::vector<InlineCacheSite>(kSites);
    std::vector<uint64_t> rows = std::vector<uint64_t>(kSites * 2, 0);
    uint64_t* table() { return reinterpret_cast<uint64_t*>(sites.data()); }
    void registerHere() { bronze_register_ic_hot(table(), rows.data(), kSites); }
    bool rowIsWay0(size_t i) const {
        uint64_t w[2];
        std::memcpy(w, &sites[i].ways[0], sizeof w);
        return rows[2 * i] == w[0] && rows[2 * i + 1] == w[1];
    }
};

Shape* shapeAt(uintptr_t a) { return reinterpret_cast<Shape*>(a); }

}  // namespace

TEST_CASE("a fill of a registered site's way 0 lands in its row, and only that") {
    Tables t;
    t.registerHere();
    t.sites[2].ways[0].fill(shapeAt(0x7000), 3, 0);
    CHECK(t.rows[4] == 0x7000);
    CHECK(t.rows[5] == 3);
    CHECK(t.rowIsWay0(2));
    // A later way is the table's alone.
    t.sites[3].ways[1].fill(shapeAt(0x8000), 4, 0);
    CHECK(t.rows[6] == 0);
    CHECK(t.rows[7] == 0);
    // Every fill kind copies both words, flags and all.
    t.sites[0].ways[0].fillAccessor(shapeAt(0x9000), 5, 1);
    CHECK(t.rowIsWay0(0));
    CHECK((t.rows[1] >> 32) == (1u | BRONZE_ABI_IC_DEPTH_ACCESSOR_FLAG));
    t.sites[1].ways[0].fillAbsent(shapeAt(0xa000));
    CHECK(t.rowIsWay0(1));
    t.sites[4].ways[0].fillForSet(shapeAt(0xb000), 6, true);
    CHECK(t.rowIsWay0(4));
    t.sites[4].ways[0].fillArrayMethod(7);
    CHECK(t.rows[8] == BRONZE_ABI_IC_SHAPE_ARRAY_METHOD);
    CHECK(t.rowIsWay0(4));
    runtime::rtIcHotClearThread();
}

TEST_CASE("move to front keeps the row on the shape now at way 0") {
    Tables t;
    t.registerHere();
    InlineCacheSite& s = t.sites[1];
    for (uintptr_t sh : {0x1000u, 0x2000u, 0x3000u, 0x4000u, 0x5000u}) {
        InlineCache* e = s.slotForInstall(shapeAt(sh));
        REQUIRE(e != nullptr);
        e->fill(shapeAt(sh), static_cast<uint32_t>(sh >> 12), 0);
        CHECK(t.rowIsWay0(1));
        CHECK(t.rows[2] == sh);
    }
    // A refill in place of a shape a later way holds leaves way 0, and the
    // row, as they were.
    InlineCache* e = s.slotForInstall(shapeAt(0x3000));
    REQUIRE(e == &s.ways[2]);
    e->fill(shapeAt(0x3000), 9, 0);
    CHECK(t.rows[2] == 0x5000);
    CHECK(t.rowIsWay0(1));
    runtime::rtIcHotClearThread();
}

TEST_CASE("an unregistered table, a forgotten range and a re-registration") {
    Tables a;
    Tables b;
    a.registerHere();
    // b never registered: its fills change no row anywhere.
    b.sites[0].ways[0].fill(shapeAt(0x1000), 1, 0);
    CHECK(b.rows[0] == 0);
    CHECK(a.rows[0] == 0);
    // A copy of the run registered later starts with the rows its sites hold.
    b.registerHere();
    CHECK(b.rowIsWay0(0));
    CHECK(runtime::rtIcHotRangeCount() == 2);
    // Registering the same table again replaces its range.
    b.registerHere();
    CHECK(runtime::rtIcHotRangeCount() == 2);
    // A dropped instance's sites stop reaching their rows.
    runtime::rtIcHotDropWithin(a.table(), kSites * sizeof(InlineCacheSite));
    CHECK(runtime::rtIcHotRangeCount() == 1);
    a.sites[0].ways[0].fill(shapeAt(0x2000), 2, 0);
    CHECK(a.rows[0] == 0);
    b.sites[0].ways[0].fill(shapeAt(0x3000), 3, 0);
    CHECK(b.rows[0] == 0x3000);
    runtime::rtIcHotClearThread();
    CHECK(runtime::rtIcHotRangeCount() == 0);
}

TEST_CASE("the runtime's own property path keeps a registered row current") {
    NonMovingArena arena;
    Heap heap;
    ShadowStackFrame frame;
    Tables t;
    t.registerHere();

    Rooted<ObjectHeader*> obj(ObjectHeader::create(heap, arena, Shape::createRoot(arena)));
    Rooted<Value> key(Value::fromString(StringHeader::createFromUTF8(heap, "k")));
    Rooted<Value> val(Value::fromDouble(4.0));
    obj.get()->setProp(heap, arena, key, val, &t.sites[0].ways[0]);
    CHECK(t.rowIsWay0(0));
    CHECK(obj.get()->getProp(heap, key, &t.sites[1]).asNumber() == 4.0);
    CHECK(t.rows[2] == reinterpret_cast<uint64_t>(obj.get()->shape));
    CHECK(t.rowIsWay0(1));
    runtime::rtIcHotClearThread();
}

// The method rows (bronze_abi_layout.h, "the METHOD ROWS"): a write to a
// registered method site's way 0 lands in the row of the site's rank in the
// method-site list, copying words 0, 1, 2 and 5; anything else is left alone.
TEST_CASE("a latch's write to a method site lands in that site's method row") {
    constexpr size_t kWords = BRONZE_ABI_IC_SITE_SIZE / sizeof(uint64_t);
    std::vector<uint64_t> table(6 * kWords, 0);
    std::vector<uint64_t> rows(3 * 4, 0);
    // Out of order on purpose: a row's index is its site's rank once sorted.
    const uint64_t sites[] = {4, 1, 3};
    for (size_t k = 0; k < 6 * kWords; ++k) table[k] = 0;
    table[3 * kWords + 0] = 0x7000;  // already latched before registration
    table[3 * kWords + BRONZE_ABI_METHOD_IC_EPOCH_WORD] = 42;
    bronze_register_method_ic_rows(table.data(), rows.data(), sites, 3);
    CHECK(runtime::rtMethodRowRangeCount() == 1);
    // Site 3 is rank 1: its row was filled at registration.
    CHECK(rows[4] == 0x7000);
    CHECK(rows[7] == 42);
    uint64_t* s4 = table.data() + 4 * kWords;
    s4[0] = 0x9000;
    s4[BRONZE_ABI_METHOD_IC_CODE_WORD] = 0x1234;
    s4[BRONZE_ABI_METHOD_IC_ARITY_WORD] = BRONZE_ABI_METHOD_IC_DIRECT_ENTRY_BIT | 2;
    s4[BRONZE_ABI_METHOD_IC_ENV_WORD] = 0xdead;
    s4[BRONZE_ABI_METHOD_IC_EPOCH_WORD] = 7;
    runtime::rtMethodRowNoteWrite(s4);
    CHECK(rows[8] == 0x9000);
    CHECK(rows[9] == 0x1234);
    CHECK(rows[10] == (BRONZE_ABI_METHOD_IC_DIRECT_ENTRY_BIT | 2));
    CHECK(rows[11] == 7);
    // A site that is no method site, and a word inside a site, change nothing.
    table[2 * kWords] = 0x5555;
    runtime::rtMethodRowNoteWrite(table.data() + 2 * kWords);
    runtime::rtMethodRowNoteWrite(s4 + 1);
    CHECK(rows[0] == 0);
    CHECK(rows[8] == 0x9000);
    // A dropped instance's range is forgotten.
    runtime::rtIcHotDropWithin(table.data(), table.size() * sizeof(uint64_t));
    CHECK(runtime::rtMethodRowRangeCount() == 0);
    s4[0] = 0xa000;
    runtime::rtMethodRowNoteWrite(s4);
    CHECK(rows[8] == 0x9000);
    runtime::rtIcHotClearThread();
}
