#include <doctest/doctest.h>

#include <brass/gc/object.hpp>
#include <brass/gc/runtime_gc.hpp>
#include <brass/gc/stack_map.hpp>

#include "abi/bronze_abi.h"
#include "codegen-brass/brass_backend.h"
#include "codegen-brass/brass_jit.h"
#include "codegen-brass/brass_tiered_engine.h"
#include "il/il.h"
#include "il/il_op.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/string.h"
#include "runtime/value.h"
#include "support/diagnostics.h"

#include <cstring>
#include <memory>
#include <vector>

using namespace bronze;

namespace {

// A heap of the test's own with one old block of Values and young values to
// store into it: the barrier generated code calls (brass_gc_write_barrier)
// must dirty the old block's card exactly when a young reference lands in it.
struct BarrierHeap {
    Heap heap;
    ShadowStackFrame frame;
    Rooted<Value> old;

    BarrierHeap() : old(heap, Value::fromUndefined()) {
        heap.set_gc_stress(false);
        auto* block = heap.allocate(sizeof(Value) * 4, Tag::Object);
        block->flags = HeapKind::ValueBlock;
        old.set(Value::fromObject(block));
        for (int i = 0; i < 4; ++i) slots()[i] = Value::fromUndefined();
        heap.collect();  // promotes the block
    }

    HeapValue* slots() { return old.get().asObject<HeapObjectHeader>()->payload<HeapValue>(); }
    uintptr_t oldAddress() { return reinterpret_cast<uintptr_t>(old.get().asObject()); }
    bool cardDirty() {
        const brass::gc::Heap& gc = heap.gc();
        return gc.card_table_base()[(oldAddress() - gc.old_base()) >> brass::gc::kCardShift] ==
               brass::gc::kCardDirty;
    }
    Value youngString(const char* text) { return Value::fromString(StringHeader::createFromUTF8(heap, text)); }
};

// A store the way generated code makes one: the raw word, then the barrier
// with the tagged object and the stored value.
void storeWithBarrier(BarrierHeap& h, size_t index, Value v) {
    reinterpret_cast<uint64_t*>(h.slots())[index] = v.rawBits();
    brass_gc_write_barrier(h.old.get().rawBits(), v.rawBits());
}

}  // namespace

TEST_CASE("write barrier - an old object storing a young reference dirties its card") {
    BarrierHeap h;
    REQUIRE_FALSE(h.heap.is_movable(h.old.get().asObject()));
    h.heap.collect_minor();
    REQUIRE_FALSE(h.cardDirty());

    const Value young = h.youngString("young");
    REQUIRE(h.heap.is_movable(young.asObject()));
    storeWithBarrier(h, 0, young);
    CHECK(h.cardDirty());

    // The minor collection finds the young string through the card, moves it
    // and updates the slot.
    h.heap.set_gc_verify(true);
    h.heap.collect_minor();
    const Value moved = h.slots()[0];
    CHECK(moved.isString());
    CHECK(moved.rawBits() != young.rawBits());
    CHECK(moved.asString<StringHeader>()->length == 5);
}

TEST_CASE("write barrier - ignores numbers, booleans and other non-references") {
    BarrierHeap h;
    h.heap.collect_minor();
    REQUIRE_FALSE(h.cardDirty());

    storeWithBarrier(h, 0, Value::fromDouble(1.5));
    storeWithBarrier(h, 1, Value::fromBool(true));
    storeWithBarrier(h, 2, Value::fromUndefined());
    storeWithBarrier(h, 3, Value::fromNull());
    CHECK_FALSE(h.cardDirty());
}

TEST_CASE("write barrier - ignores young-to-young and old-to-old stores") {
    BarrierHeap h;
    h.heap.collect_minor();

    // Old into old: a reference, but nothing a minor collection moves.
    storeWithBarrier(h, 0, h.old.get());
    CHECK_FALSE(h.cardDirty());

    // Young into young: the young object is scanned by the minor collection
    // anyway.
    auto* youngBlock = h.heap.allocate(sizeof(Value), Tag::Object);
    youngBlock->flags = HeapKind::ValueBlock;
    const Value young = h.youngString("y");
    reinterpret_cast<uint64_t*>(youngBlock->payload())[0] = young.rawBits();
    brass_gc_write_barrier(Value::fromObject(youngBlock).rawBits(), young.rawBits());
    CHECK_FALSE(h.cardDirty());
}

TEST_CASE("write barrier - HeapValue stores and bulk copies are barriered") {
    BarrierHeap h;
    h.heap.set_gc_verify(true);
    h.heap.collect_minor();
    REQUIRE_FALSE(h.cardDirty());

    h.slots()[0] = h.youngString("assigned");
    CHECK(h.cardDirty());
    h.heap.collect_minor();
    CHECK(h.slots()[0].asString<StringHeader>()->length == 8);

    h.heap.collect_minor();
    Value run[2] = {h.youngString("copied"), Value::fromDouble(2.0)};
    gcCopyValues(h.old.get().asObject(), h.slots() + 1, run, 2);
    CHECK(h.cardDirty());
    h.heap.collect_minor();
    CHECK(h.slots()[1].asString<StringHeader>()->length == 6);
    CHECK(h.slots()[2].asNumber() == 2.0);
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
