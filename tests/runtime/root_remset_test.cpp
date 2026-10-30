// The root tables' remembered sets (runtime/root_remset.h).
//
// Two properties, asked of the tables directly:
//   - a store of a young object through each table's writer is seen by the
//     next minor collection: the cell is forwarded, and the verification walk
//     (the one a minor runs under BRASS_GC_VERIFY=1) finds no young cell off
//     its set, before the minor or after it;
//   - a quiet minor visits nothing from the sets, and a minor after one write
//     visits about that one cell, not the table it lives in.
// And the net under them: a cell written behind its writer's back is named by
// the verification walk, which is what makes a missed writer a hard error.

#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/host_globals.h"
#include "runtime/object.h"
#include "runtime/root_remset.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"
#include "runtime/weak_ref.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

constexpr uint64_t kUndefined = BRONZE_ABI_UNDEFINED_BITS;
constexpr uint64_t kSiteWords = BRONZE_ABI_IC_SITE_SIZE / sizeof(uint64_t);

// Module data stand-ins: static, so a registration (which is for the
// thread's life) never outlives them.
uint64_t g_templateCells[2048];
uint64_t g_cacheCells[4];
uint64_t g_fnSlots[4];
uint64_t g_icTable[kSiteWords * 2];

void registerTables() {
    static bool done = false;
    if (done) return;
    done = true;
    for (uint64_t& c : g_templateCells) c = kUndefined;
    for (uint64_t& c : g_cacheCells) c = BRONZE_ABI_HOLE_BITS;
    for (uint64_t& c : g_fnSlots) c = 0;
    for (uint64_t& c : g_icTable) c = 0;
    g_icTable[kSiteWords + BRONZE_ABI_METHOD_IC_ENV_WORD] = kUndefined;
    g_icTable[kSiteWords + BRONZE_ABI_METHOD_IC_WAY1_ENV_WORD] = kUndefined;
    bronze_register_template_cells(g_templateCells, std::size(g_templateCells));
    bronze_register_fn_slots(g_fnSlots, 2);
    static const uint64_t sites[1] = {1};
    bronze_register_method_ic_cells(g_icTable, sites, 1);
}

// An object with one own property holding a number, so a cell that survives a
// collection can be shown to name a live object and not merely an address.
Value tagged(double n) {
    Rooted<Value> root{Value(bronze_create_object())};
    const uint32_t key = bronze_register_key_string("v");
    bronze_prop_set(root.get().rawBits(), key, Value::fromDouble(n).rawBits(), nullptr, false);
    return root.get();
}

double tagOf(Value v) {
    Rooted<Value> root{v};
    const uint32_t key = bronze_register_key_string("v");
    return Value(bronze_prop_get(root.get().rawBits(), key, nullptr)).asNumber();
}

uint64_t visits() { return rtRootRemsetStats().rememberedVisits; }

// Every set is empty after a full collection: it promotes everything young.
void settle() { rtHeap().collect(); }

uint64_t probeNative(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return Value::fromDouble(1234.5).rawBits();
}

}  // namespace

TEST_CASE("a quiet minor visits no remembered cell, and one write costs about one visit") {
    ShadowStackFrame frame;
    registerTables();
    settle();
    REQUIRE(rtRootRemsetCheck().empty());

    // Nothing has been allocated since the full collection: nothing is young,
    // so no table has a cell for the minor to visit, however large it is.
    uint64_t before = visits();
    rtHeap().collect_minor();
    CHECK(visits() == before);

    // One template object, written through its one writer into one cell of
    // a 2048-cell table.
    Rooted<Value> cooked{Value(bronze_create_array(1))};
    Rooted<Value> raw{Value(bronze_create_array(1))};
    bronze_template_object(cooked.get().rawBits(), raw.get().rawBits(), &g_templateCells[700]);
    CHECK(rtRootRemsetCheck().empty());
    const bool wasYoung = gcHoldsYoung(Value(g_templateCells[700]));
    before = visits();
    rtHeap().collect_minor();
    if (wasYoung && !rtHeap().gc_stress()) {
        // That cell, and at most a handful of others the template object's
        // freezing made young (a prototype shape, say) — not the table.
        CHECK(visits() - before >= 1);
        CHECK(visits() - before < 16);
    }
    CHECK(g_templateCells[700] == cooked.get().rawBits());
    CHECK(rtRootRemsetCheck().empty());

    // Two more minors age the object out of the young generation (tenure age
    // 2), and the cell leaves the set: the next quiet minor visits nothing.
    rtHeap().collect_minor();
    rtHeap().collect_minor();
    CHECK_FALSE(gcHoldsYoung(Value(g_templateCells[700])));
    before = visits();
    rtHeap().collect_minor();
    CHECK(visits() == before);
    CHECK(g_templateCells[700] == cooked.get().rawBits());
}

TEST_CASE("a young object stored through each table's writer is forwarded by the next minor") {
    ShadowStackFrame frame;
    registerTables();
    settle();

    // Template cell.
    Rooted<Value> cooked{Value(bronze_create_array(1))};
    Rooted<Value> raw{Value(bronze_create_array(1))};
    bronze_template_object(cooked.get().rawBits(), raw.get().rawBits(), &g_templateCells[11]);

    // Global cache cell: the writer resolves a host global and caches it.
    Rooted<Value> hostValue{tagged(5.0)};
    rtRegisterHostGlobal("remsetProbeGlobal", hostValue.get());
    const uint32_t globalKey = bronze_register_key_string("remsetProbeGlobal");
    bronze_global_get_cached(globalKey, g_cacheCells, std::size(g_cacheCells), 2);
    CHECK(g_cacheCells[2] == hostValue.get().rawBits());

    // Fn slot (and the function singleton table behind it): a fresh code
    // pointer, so the singleton is made here, young.
    const Value fn{bronze_function_singleton(
        probeNative, 0, 0, BRONZE_ABI_FN_NAME_NONE,
        BRONZE_ABI_FN_FLAGS_ORDINARY | BRONZE_ABI_FN_FLAG_NATIVE, g_fnSlots)};
    Rooted<Value> fnRoot{fn};
    CHECK(g_fnSlots[1] == fnRoot.get().rawBits());

    // A method site's env word, as a latch leaves it (rt_method_call.cpp).
    Rooted<Value> env{tagged(7.0)};
    uint64_t* site = g_icTable + kSiteWords;
    site[BRONZE_ABI_METHOD_IC_ENV_WORD] = env.get().rawBits();
    site[BRONZE_ABI_METHOD_IC_WAY1_ENV_WORD] = env.get().rawBits();
    rtNoteMethodIcEnvWords(site);

    // A root shape and a user prototype shape, each over a young prototype.
    Rooted<Value> rootProto{tagged(8.0)};
    Shape* rootShape = rtNewRootShape(rootProto.get());
    Rooted<Value> userProto{tagged(9.0)};
    Shape* userShape = rtRootShapeForPrototype(userProto.get());

    // A FinalizationRegistry whose cell holds a young held value nothing
    // else names.
    Rooted<Value> cb{rtNativeFunction(probeNative, 1)};
    Rooted<Value> reg{rtNewFinalizationRegistry(cb)};
    Rooted<Value> target{tagged(10.0)};
    {
        Rooted<Value> held{tagged(11.0)};
        Rooted<Value> noToken{Value::fromUndefined()};
        rtFinalizationRegister(reg, target, held, noToken);
    }

    // A tracked buffer and a view over it, both young.
    Shape* plain = rtPlainObjectShape();
    Rooted<Value> view{Value::fromObject(
        TypedArrayHeader::create(rtHeap(), plain, plain, ElementKind::Uint8, 8))};
    {
        auto* buf = view.get().asObject<TypedArrayHeader>()->buffer.asObject<ArrayBufferHeader>();
        trackBufferViews(rtHeap(), buf);
        registerBufferView(view.get().asObject<TypedArrayHeader>());
    }

    // Every writer above noted what it stored.
    CHECK(rtRootRemsetCheck().empty());

    for (int round = 0; round < 3; ++round) {
        CAPTURE(round);
        rtHeap().collect_minor();
        // No cell holds a young address off its set: a missed cell would
        // still hold its object's from-space address, which is young.
        CHECK(rtRootRemsetCheck().empty());
        CHECK(g_templateCells[11] == cooked.get().rawBits());
        CHECK(g_cacheCells[2] == hostValue.get().rawBits());
        CHECK(g_fnSlots[1] == fnRoot.get().rawBits());
        CHECK(Value(bronze_function_singleton(probeNative, 0, 0, BRONZE_ABI_FN_NAME_NONE,
                                              BRONZE_ABI_FN_FLAGS_ORDINARY |
                                                  BRONZE_ABI_FN_FLAG_NATIVE,
                                              nullptr))
                  .rawBits() == fnRoot.get().rawBits());
        CHECK(site[BRONZE_ABI_METHOD_IC_ENV_WORD] == env.get().rawBits());
        CHECK(site[BRONZE_ABI_METHOD_IC_WAY1_ENV_WORD] == env.get().rawBits());
        CHECK(rootShape->prototype.rawBits() == rootProto.get().rawBits());
        CHECK(userShape->prototype.rawBits() == userProto.get().rawBits());
        CHECK(tagOf(rootShape->prototype) == 8.0);
        CHECK(tagOf(userShape->prototype) == 9.0);
        CHECK(view.get().asObject<TypedArrayHeader>()->length == 8);
    }
    // The held value lived only in the registry's cell; a minor that skipped
    // the block would have left its from-space address there, which the
    // checks above name. The cell itself is still registered.
    CHECK(rtFinalizationCellCount() >= 1);
    rtClearKeptObjects();
    rtHeap().collect();
    CHECK(rtRootRemsetCheck().empty());
}

TEST_CASE("a cell written behind its writer's back is named by the verification walk") {
    ShadowStackFrame frame;
    registerTables();
    settle();

    Rooted<Value> young{tagged(3.0)};
    if (!gcHoldsYoung(young.get())) return;  // a stress collection promoted it already
    g_templateCells[900] = young.get().rawBits();
    const std::string miss = rtRootRemsetCheck();
    CHECK(miss.find("template") != std::string::npos);
    CHECK(miss.find("cell 900") != std::string::npos);
    // Put right before anything can collect: the minor under
    // BRASS_GC_VERIFY=1 would stop the process with this same message.
    g_templateCells[900] = kUndefined;
    CHECK(rtRootRemsetCheck().empty());
}

TEST_CASE("BRONZE_NO_ROOT_REMSET's switch visits everything and keeps the sets exact") {
    ShadowStackFrame frame;
    registerTables();
    settle();

    rtSetRootRemsetEnabled(false);
    Rooted<Value> cooked{Value(bronze_create_array(1))};
    Rooted<Value> raw{Value(bronze_create_array(1))};
    bronze_template_object(cooked.get().rawBits(), raw.get().rawBits(), &g_templateCells[1500]);
    rtHeap().collect_minor();
    CHECK(g_templateCells[1500] == cooked.get().rawBits());
    rtSetRootRemsetEnabled(true);

    // Switched back on mid-run, the sets still hold what the writers noted
    // and the minors pruned.
    CHECK(rtRootRemsetCheck().empty());
    rtHeap().collect_minor();
    CHECK(g_templateCells[1500] == cooked.get().rawBits());
    CHECK(rtRootRemsetCheck().empty());
}
