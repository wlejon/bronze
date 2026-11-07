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
