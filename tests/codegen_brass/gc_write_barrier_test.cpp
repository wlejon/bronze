#include <doctest/doctest.h>

#include <brass/gc/card_table.hpp>
#include <brass/gc/generational_gc.hpp>
#include <brass/gc/runtime_gc.hpp>
#include <brass/gc/stack_map.hpp>

#include "abi/bronze_abi.h"
#include "codegen-brass/brass_backend.h"
#include "codegen-brass/brass_jit.h"
#include "codegen-brass/brass_tiered_engine.h"
#include "il/il.h"
#include "il/il_op.h"
#include "runtime/gc.h"
#include "runtime/value.h"
#include "support/diagnostics.h"

#include <cstring>
#include <memory>
#include <vector>

using namespace bronze;

TEST_CASE("write barrier - GenerationalGC old stores young marks CardTable (raw and NaN-tagged)") {
    brass::brass_set_active_generational_gc(nullptr);
    set_active_card_table(nullptr);

    brass::GenerationalGC gc(64 * 1024, 32 * 1024, 128 * 1024);
    gc.set_tenuring_threshold(2);
    brass::brass_set_active_generational_gc(&gc);

    // Allocate an object in nursery and tenure it through two collections
    uintptr_t old_obj = gc.allocate(32, 1ULL, 10);
    gc.write_field(old_obj, 0, 0);
    uintptr_t root = old_obj;
    std::vector<uintptr_t*> roots = {&root};
    gc.collect(roots); // Nursery -> Survivor
    gc.collect(roots); // Survivor -> Tenured
    old_obj = root;
    REQUIRE(gc.is_in_tenured(old_obj));

    // Allocate young object in nursery
    uintptr_t young_obj = gc.allocate(32, 0ULL, 20);
    REQUIRE(gc.is_in_nursery(young_obj));

    // 1. Raw pointers: old -> young
    gc.card_table().clean_all();
    reset_write_barrier_stats();
    CHECK(!gc.card_table().is_dirty_addr(old_obj));

    brass_gc_write_barrier(old_obj, young_obj);

    CHECK(gc.card_table().is_dirty_addr(old_obj));
    CHECK_EQ(get_write_barrier_stats().total_invocations, 1ULL);
    CHECK_EQ(get_write_barrier_stats().old_to_young_marked, 1ULL);
    CHECK_EQ(get_write_barrier_stats().filtered_non_old_obj, 0ULL);
    CHECK_EQ(get_write_barrier_stats().filtered_non_young_val, 0ULL);
    CHECK_EQ(get_write_barrier_stats().filtered_non_pointer, 0ULL);

    // 2. NaN-tagged pointers: Tag::Object
    gc.card_table().clean_all();
    reset_write_barrier_stats();

    const uint64_t tagged_old = (static_cast<uint64_t>(Tag::Object) << kTagShift) | old_obj;
    const uint64_t tagged_young = (static_cast<uint64_t>(Tag::Object) << kTagShift) | young_obj;

    brass_gc_write_barrier(tagged_old, tagged_young);

    CHECK(gc.card_table().is_dirty_addr(old_obj));
    CHECK_EQ(get_write_barrier_stats().total_invocations, 1ULL);
    CHECK_EQ(get_write_barrier_stats().old_to_young_marked, 1ULL);

    // 3. Mixed: Tagged old -> Raw young
    gc.card_table().clean_all();
    reset_write_barrier_stats();

    brass_gc_write_barrier(tagged_old, young_obj);

    CHECK(gc.card_table().is_dirty_addr(old_obj));
    CHECK_EQ(get_write_barrier_stats().old_to_young_marked, 1ULL);

    // 4. Mixed: Raw old -> Tagged young
    gc.card_table().clean_all();
    reset_write_barrier_stats();

    brass_gc_write_barrier(old_obj, tagged_young);

    CHECK(gc.card_table().is_dirty_addr(old_obj));
    CHECK_EQ(get_write_barrier_stats().old_to_young_marked, 1ULL);

    // 5. String tag: Tag::String
    gc.card_table().clean_all();
    reset_write_barrier_stats();

    const uint64_t str_young = (static_cast<uint64_t>(Tag::String) << kTagShift) | young_obj;
    brass_gc_write_barrier(old_obj, str_young);

    CHECK(gc.card_table().is_dirty_addr(old_obj));
    CHECK_EQ(get_write_barrier_stats().old_to_young_marked, 1ULL);

    // 6. BigInt tag: Tag::BigInt
    gc.card_table().clean_all();
    reset_write_barrier_stats();

    const uint64_t bigint_young = (static_cast<uint64_t>(Tag::BigInt) << kTagShift) | young_obj;
    brass_gc_write_barrier(old_obj, bigint_young);

    CHECK(gc.card_table().is_dirty_addr(old_obj));
    CHECK_EQ(get_write_barrier_stats().old_to_young_marked, 1ULL);

    brass::brass_set_active_generational_gc(nullptr);
}

TEST_CASE("write barrier - ignores non-pointer and scalar values") {
    brass::brass_set_active_generational_gc(nullptr);
    set_active_card_table(nullptr);

    brass::GenerationalGC gc(64 * 1024, 32 * 1024, 128 * 1024);
    gc.set_tenuring_threshold(2);
    brass::brass_set_active_generational_gc(&gc);

    uintptr_t old_obj = gc.allocate(32, 1ULL, 10);
    gc.write_field(old_obj, 0, 0);
    uintptr_t root = old_obj;
    std::vector<uintptr_t*> roots = {&root};
    gc.collect(roots);
    gc.collect(roots);
    old_obj = root;
    REQUIRE(gc.is_in_tenured(old_obj));

    gc.card_table().clean_all();
    reset_write_barrier_stats();

    // Floating-point IEEE-754 numbers (doubles)
    brass_gc_write_barrier(old_obj, Value::fromDouble(0.0).rawBits());
    brass_gc_write_barrier(old_obj, Value::fromDouble(-0.0).rawBits());
    brass_gc_write_barrier(old_obj, Value::fromDouble(42.0).rawBits());
    brass_gc_write_barrier(old_obj, Value::fromDouble(3.1415926535).rawBits());
    brass_gc_write_barrier(old_obj, Value::fromDouble(-1e20).rawBits());
    brass_gc_write_barrier(old_obj, Value(kCanonicalNaNBits).rawBits());

    // Booleans
    brass_gc_write_barrier(old_obj, Value::fromBool(true).rawBits());
    brass_gc_write_barrier(old_obj, Value::fromBool(false).rawBits());

    // Undefined & Null
    brass_gc_write_barrier(old_obj, Value::fromUndefined().rawBits());
    brass_gc_write_barrier(old_obj, Value::fromNull().rawBits());

    // Int32 scalar
    brass_gc_write_barrier(old_obj, Value::fromTagAndPayload(static_cast<uint16_t>(Tag::Int32), 12345).rawBits());

    // Hole & Uninitialized
    brass_gc_write_barrier(old_obj, Value::fromHole().rawBits());
    brass_gc_write_barrier(old_obj, Value::fromUninitialized().rawBits());

    // Zero / null pointer
    brass_gc_write_barrier(old_obj, 0);

    CHECK(!gc.card_table().is_dirty_addr(old_obj));
    CHECK_EQ(get_write_barrier_stats().old_to_young_marked, 0ULL);
    CHECK_EQ(get_write_barrier_stats().filtered_non_pointer, 14ULL);

    brass::brass_set_active_generational_gc(nullptr);
}

TEST_CASE("write barrier - ignores young-to-young and old-to-old writes") {
    brass::brass_set_active_generational_gc(nullptr);
    set_active_card_table(nullptr);

    brass::GenerationalGC gc(64 * 1024, 32 * 1024, 128 * 1024);
    gc.set_tenuring_threshold(2);
    brass::brass_set_active_generational_gc(&gc);

    // Two old objects
    uintptr_t obj1 = gc.allocate(32, 1ULL, 1);
    uintptr_t obj2 = gc.allocate(32, 1ULL, 2);
    uintptr_t r1 = obj1, r2 = obj2;
    std::vector<uintptr_t*> roots = {&r1, &r2};
    gc.collect(roots);
    gc.collect(roots);
    uintptr_t old1 = r1;
    uintptr_t old2 = r2;
    REQUIRE(gc.is_in_tenured(old1));
    REQUIRE(gc.is_in_tenured(old2));

    // Two young objects
    uintptr_t young1 = gc.allocate(32, 0ULL, 3);
    uintptr_t young2 = gc.allocate(32, 0ULL, 4);
    REQUIRE(gc.is_in_nursery(young1));
    REQUIRE(gc.is_in_nursery(young2));

    // 1. Young-to-young write
    gc.card_table().clean_all();
    reset_write_barrier_stats();

    brass_gc_write_barrier(young1, young2);

    CHECK(!gc.card_table().is_dirty_addr(young1));
    CHECK_EQ(get_write_barrier_stats().old_to_young_marked, 0ULL);
    CHECK_EQ(get_write_barrier_stats().filtered_non_old_obj, 1ULL);

    // 2. Young-to-old write
    reset_write_barrier_stats();

    brass_gc_write_barrier(young1, old1);

    CHECK(!gc.card_table().is_dirty_addr(young1));
    CHECK_EQ(get_write_barrier_stats().old_to_young_marked, 0ULL);
    CHECK_EQ(get_write_barrier_stats().filtered_non_old_obj, 1ULL);

    // 3. Old-to-old write
    reset_write_barrier_stats();

    brass_gc_write_barrier(old1, old2);

    CHECK(!gc.card_table().is_dirty_addr(old1));
    CHECK_EQ(get_write_barrier_stats().old_to_young_marked, 0ULL);
    CHECK_EQ(get_write_barrier_stats().filtered_non_young_val, 1ULL);

    brass::brass_set_active_generational_gc(nullptr);
}

TEST_CASE("write barrier - active Bronze CardTable descriptor integration") {
    brass::brass_set_active_generational_gc(nullptr);

    constexpr size_t HEAP_SZ = 64 * 1024;
    std::vector<uint8_t> heap(HEAP_SZ);
    const uintptr_t base = reinterpret_cast<uintptr_t>(heap.data());
    brass::CardTable ct(base, HEAP_SZ);

    const uintptr_t old_base = base;
    const size_t old_size = 32 * 1024;
    const uintptr_t young_base = base + 32 * 1024;
    const size_t young_size = 32 * 1024;

    set_active_card_table(&ct, old_base, old_size, young_base, young_size);
    CHECK_EQ(get_active_card_table(), &ct);

    const uintptr_t old_ptr = old_base + 1024;
    const uintptr_t young_ptr = young_base + 1024;

    // Old stores young marks card table
    reset_write_barrier_stats();
    brass_gc_write_barrier(old_ptr, young_ptr);
    CHECK(ct.is_dirty_addr(old_ptr));
    CHECK_EQ(get_write_barrier_stats().old_to_young_marked, 1ULL);

    // Tagged pointers
    ct.clean_all();
    reset_write_barrier_stats();
    const uint64_t tagged_old = (static_cast<uint64_t>(Tag::Object) << kTagShift) | old_ptr;
    const uint64_t tagged_young = (static_cast<uint64_t>(Tag::Object) << kTagShift) | young_ptr;
    brass_gc_write_barrier(tagged_old, tagged_young);
    CHECK(ct.is_dirty_addr(old_ptr));
    CHECK_EQ(get_write_barrier_stats().old_to_young_marked, 1ULL);

    // Young-to-young ignores
    ct.clean_all();
    reset_write_barrier_stats();
    brass_gc_write_barrier(young_ptr, young_ptr + 64);
    CHECK(!ct.is_dirty_addr(young_ptr));
    CHECK_EQ(get_write_barrier_stats().filtered_non_old_obj, 1ULL);

    // Predicate-based configuration
    static uintptr_t s_base = 0;
    s_base = base;
    set_active_card_table(&ct,
        [](uintptr_t a) { return a == s_base + 0x1000; },
        [](uintptr_t a) { return a == s_base + 0x2000; });
    ct.clean_all();
    reset_write_barrier_stats();

    brass_gc_write_barrier(base + 0x1000, base + 0x2000);
    CHECK(ct.is_dirty_addr(base + 0x1000));
    CHECK_EQ(get_write_barrier_stats().old_to_young_marked, 1ULL);

    set_active_card_table(nullptr);
    CHECK_EQ(get_active_card_table(), nullptr);
}

namespace {
il::Module createSampleModule(std::string name) {
    il::Module m;
    m.name = std::move(name);

    // compute(x: f64) -> f64: x + 10.0
    il::Function fn;
    fn.name = "compute";
    fn.params = {{"x", il::Type::F64}};
    fn.returnType = il::Type::F64;
    fn.isExported = true;
    fn.valueCount = 3;
    il::Block b0;
    b0.id = 0;
    b0.instructions.push_back({il::Op::ConstF64, il::Type::F64, 1, {}, 10.0, 0, 0});
    il::Instruction addInst;
    addInst.op = il::Op::Add;
    addInst.type = il::Type::F64;
    addInst.result = 2;
    addInst.operands = {0, 1};
    b0.instructions.push_back(addInst);
    b0.instructions.push_back({il::Op::Ret, il::Type::F64, il::kNoValue, {2}, 0, 0, 0});
    fn.blocks.push_back(std::move(b0));
    m.functions.push_back(std::move(fn));

    // main() -> void
    il::Function mainFn;
    mainFn.name = "main";
    mainFn.isEntryPoint = true;
    mainFn.returnType = il::Type::Void;
    mainFn.valueCount = 3;
    il::Block mb0;
    mb0.id = 0;
    mb0.instructions.push_back({il::Op::ConstF64, il::Type::F64, 0, {}, 16.0, 0, 0});
    il::Instruction callInst;
    callInst.op = il::Op::Call;
    callInst.type = il::Type::F64;
    callInst.result = 1;
    callInst.operands = {0};
    callInst.calleeIndex = 0;
    mb0.instructions.push_back(callInst);
    mb0.instructions.push_back({il::Op::Ret, il::Type::Void, il::kNoValue, {}, 0, 0, 0});
    mainFn.blocks.push_back(std::move(mb0));
    m.functions.push_back(std::move(mainFn));

    return m;
}
} // namespace

TEST_CASE("binary stack maps (BSCM) - JIT execution engine registers and cleans ModuleStackMap") {
    brass::brass_set_active_stack_maps(nullptr);

    BrassBackend backend;
    backend.setEntrySymbol("main");
    backend.setOptimize(true);

    DiagnosticSink diags;
    il::Module m = createSampleModule("jit_bscm_mod");
    auto prog = backend.compileToJit(m, diags);

    REQUIRE(!diags.hasErrors());
    REQUIRE(prog != nullptr);

    // Verify stackMaps() is accessible and populated
    const auto* stackMaps = prog->stackMaps();
    REQUIRE(stackMaps != nullptr);

    // Active stack maps in runtime GC bridge must point to prog->stackMaps()
    CHECK_EQ(brass::brass_get_active_stack_maps(), stackMaps);

    // Verify binary serialization round-trip using BSCM format
    std::vector<uint8_t> encoded = brass::encode_stack_maps(*stackMaps);
    CHECK(encoded.size() >= 12);

    // Verify BSCM magic header: 0x4D435342 ("BSCM" in little-endian)
    uint32_t magic = 0;
    std::memcpy(&magic, encoded.data(), sizeof(uint32_t));
    CHECK_EQ(magic, brass::STACK_MAP_MAGIC);

    // Decode stack maps
    brass::ModuleStackMap decoded = brass::decode_stack_maps(encoded);
    CHECK_EQ(decoded.size(), stackMaps->size());

    // Execute program
    prog->run();

    // On destruction, active stack maps must be cleaned up (reset to nullptr)
    prog.reset();
    CHECK_EQ(brass::brass_get_active_stack_maps(), nullptr);
}

TEST_CASE("binary stack maps (BSCM) - TieredEngine baseline and auto tiers manage ModuleStackMap") {
    brass::brass_set_active_stack_maps(nullptr);

    // 1. Tier 1: Baseline JIT Compiler
    {
        TieredEngineConfig config;
        config.tier = ExecutionTier::Tier1_Baseline;
        config.entrySymbol = "main";

        BrassTieredEngine engine(config);
        DiagnosticSink diags;
        il::Module m = createSampleModule("baseline_bscm_mod");
        auto prog = engine.compile(m, diags);

        REQUIRE(!diags.hasErrors());
        REQUIRE(prog != nullptr);

        const auto* stackMaps = prog->stackMaps();
        REQUIRE(stackMaps != nullptr);
        CHECK_EQ(brass::brass_get_active_stack_maps(), stackMaps);
        CHECK(!stackMaps->empty());

        // Verify BSCM encoding
        std::vector<uint8_t> encoded = brass::encode_stack_maps(*stackMaps);
        CHECK(encoded.size() >= 12);
        uint32_t magic = 0;
        std::memcpy(&magic, encoded.data(), sizeof(uint32_t));
        CHECK_EQ(magic, brass::STACK_MAP_MAGIC);

        auto invRes = prog->invoke("compute", {brass::RuntimeValue::from_f64(5.0)});
        CHECK(invRes.as_f64() == doctest::Approx(15.0));

        prog.reset();
        CHECK_EQ(brass::brass_get_active_stack_maps(), nullptr);
    }

    // 2. Tier 3: Auto Tiering Pipeline
    {
        TieredEngineConfig config;
        config.tier = ExecutionTier::Auto;
        config.entrySymbol = "main";

        BrassTieredEngine engine(config);
        DiagnosticSink diags;
        il::Module m = createSampleModule("auto_bscm_mod");
        auto prog = engine.compile(m, diags);

        REQUIRE(!diags.hasErrors());
        REQUIRE(prog != nullptr);

        const auto* stackMaps = prog->stackMaps();
        REQUIRE(stackMaps != nullptr);
        CHECK_EQ(brass::brass_get_active_stack_maps(), stackMaps);

        prog.reset();
        CHECK_EQ(brass::brass_get_active_stack_maps(), nullptr);
    }
}
