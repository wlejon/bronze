#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include <brass/codegen/baseline_jit.hpp>
#include <brass/codegen/jit_exec.hpp>
#include <brass/gc/generational_gc.hpp>
#include <brass/gc/runtime_gc.hpp>
#include <brass/vm/bytecode.hpp>
#include <brass/vm/fast_interpreter.hpp>
#include <brass/mir/builder.hpp>
#include <brass/mir/coro_transform.hpp>
#include <brass/mir/module.hpp>
#include <brass/mir/opcodes.hpp>
#include <brass/mir/verifier.hpp>
#include <brass/runtime/coroutine.hpp>

#include "codegen-brass/brass_coroutine_bridge.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/value.h"

using namespace bronze;

TEST_CASE("coroutine - generator state machine transformation and iteration") {
    brass::Module mod("coro_gen_test");
    bronze::NativeCoroBuilder coroBuilder(mod);

    brass::Function* genFn = coroBuilder.createCoroFunction("my_generator", brass::Type::i64());
    brass::Builder b(mod);
    b.set_function(genFn);

    brass::BasicBlock* entry = b.append_block("entry");
    b.position_at_end(entry);
    b.add_block_param(entry, brass::Type::gcref());

    // First yield: 111
    brass::Value* v1 = b.build_iconst_i64(111);
    coroBuilder.emitYield(b, v1, 1);

    // Second yield: 222
    brass::Value* v2 = b.build_iconst_i64(222);
    coroBuilder.emitYield(b, v2, 2);

    // Return: 333
    brass::Value* v3 = b.build_iconst_i64(333);
    b.build_ret(v3);

    // Verify coroutine opcode detection
    CHECK(bronze::functionContainsCoroOpcodes(*genFn));
    CHECK(bronze::moduleContainsCoroOpcodes(mod));

    // Transform into state machine
    brass::CoroTransformStats stats;
    brass::CoroTransformOptions opts;
    opts.stats = &stats;
    bool changed = bronze::runCoroTransformPass(*genFn, opts);
    CHECK(changed);
    CHECK_EQ(stats.coroutines_transformed, 1ULL);
    CHECK_EQ(stats.suspend_points_transformed, 2ULL);

    // Entry block must be bb_coro_entry with a switch
    CHECK_EQ(genFn->entry_block()->name(), "bb_coro_entry");
    CHECK(genFn->entry_block()->tail() != nullptr);
    CHECK_EQ(genFn->entry_block()->tail()->opcode(), brass::Opcode::switch_);

    // Check resume points registered
    CHECK_EQ(genFn->resume_points().size(), 2ULL);

    // Compile and run via JIT
    brass::codegen::JitExecutionEngine jit(brass::Target::host());
    bronze::registerBrassCoroutineSymbols(jit);
    jit.compile_and_load(mod);

    void* fnPtr = jit.get_symbol_address("my_generator");
    REQUIRE(fnPtr != nullptr);

    uintptr_t frameAddr = bronze::NativeCoroBuilder::createInstance(fnPtr, 16, 0);
    REQUIRE(frameAddr != 0);

    // Step 1: yields 111
    auto r1 = bronze::NativeCoroBuilder::stepGenerator(frameAddr);
    CHECK_EQ(r1.value, 111ULL);
    CHECK_FALSE(r1.done);
    CHECK_FALSE(bronze::NativeCoroBuilder::isDone(frameAddr));

    // Step 2: yields 222
    auto r2 = bronze::NativeCoroBuilder::stepGenerator(frameAddr);
    CHECK_EQ(r2.value, 222ULL);
    CHECK_FALSE(r2.done);
    CHECK_FALSE(bronze::NativeCoroBuilder::isDone(frameAddr));

    // Step 3: return 333
    auto r3 = bronze::NativeCoroBuilder::stepGenerator(frameAddr);
    CHECK_EQ(r3.value, 333ULL);
    CHECK(r3.done);
    CHECK(bronze::NativeCoroBuilder::isDone(frameAddr));

    bronze::NativeCoroBuilder::destroyInstance(frameAddr);
}

TEST_CASE("coroutine - SSA live variable preservation across multiple yields") {
    brass::Module mod("coro_ssa_live_test");
    bronze::NativeCoroBuilder coroBuilder(mod);

    brass::Function* fn = coroBuilder.createCoroFunction("live_spill_fn", brass::Type::i64());
    brass::Builder b(mod);
    b.set_function(fn);

    brass::BasicBlock* entry = b.append_block("entry");
    b.position_at_end(entry);
    b.add_block_param(entry, brass::Type::gcref());

    // Compute multiple base SSA values live across suspend 1
    brass::Value* baseA = b.build_iconst_i64(100);
    brass::Value* baseB = b.build_iconst_i64(200);

    // Yield 1: yields baseA (100), spilling baseA and baseB
    coroBuilder.emitYield(b, baseA, 1);

    // Compute intermediate SSA value using both spilled variables
    brass::Value* midC = b.build_add(baseA, baseB); // 300

    // Yield 2: yields midC (300), spilling midC
    coroBuilder.emitYield(b, midC, 2);

    // Final result uses spilled midC across suspend 2
    brass::Value* finalVal = b.build_add(midC, b.build_iconst_i64(50)); // 350
    b.build_ret(finalVal);

    brass::CoroTransformStats stats;
    brass::CoroTransformOptions opts;
    opts.stats = &stats;
    bool changed = bronze::runCoroTransformPass(*fn, opts);
    CHECK(changed);
    CHECK_EQ(stats.coroutines_transformed, 1ULL);
    CHECK_EQ(stats.suspend_points_transformed, 2ULL);
    CHECK_GE(stats.variables_spilled, 2ULL);

    brass::codegen::JitExecutionEngine jit(brass::Target::host());
    bronze::registerBrassCoroutineSymbols(jit);
    jit.compile_and_load(mod);

    void* fnPtr = jit.get_symbol_address("live_spill_fn");
    REQUIRE(fnPtr != nullptr);

    uintptr_t frameAddr = bronze::NativeCoroBuilder::createInstance(fnPtr, 32, 0);
    REQUIRE(frameAddr != 0);

    // Step 1: expect 100
    auto r1 = bronze::NativeCoroBuilder::stepGenerator(frameAddr);
    CHECK_EQ(r1.value, 100ULL);
    CHECK_FALSE(r1.done);

    // Step 2: expect 300 (baseA + baseB preserved across suspend 1)
    auto r2 = bronze::NativeCoroBuilder::stepGenerator(frameAddr);
    CHECK_EQ(r2.value, 300ULL);
    CHECK_FALSE(r2.done);

    // Step 3: expect 350 (midC preserved across suspend 2)
    auto r3 = bronze::NativeCoroBuilder::stepGenerator(frameAddr);
    CHECK_EQ(r3.value, 350ULL);
    CHECK(r3.done);

    bronze::NativeCoroBuilder::destroyInstance(frameAddr);
}

TEST_CASE("coroutine - frame GC root tracing and relocation during moving GC") {
    Heap heap(128 * 1024);
    ShadowStackFrame shadowFrame;
    bronze::registerCoroFrameGcRootSource(heap);

    // Allocate an object in Bronze heap
    HeapObjectHeader* hdr = heap.allocate(sizeof(HeapObjectHeader) + 16, Tag::Object);
    hdr->flags = HeapKind::ValueBlock;
    uint64_t* payload = reinterpret_cast<uint64_t*>(hdr->payload());
    payload[0] = 0xDEADBEEFCAFEULL;

    const uintptr_t initialAddr = reinterpret_cast<uintptr_t>(hdr);
    const Value initialVal = Value::fromObject(hdr);

    // Create a coroutine frame
    uintptr_t frameAddr = bronze::NativeCoroBuilder::createInstance(nullptr, 8, 0);
    REQUIRE(frameAddr != 0);
    auto* frame = reinterpret_cast<brass::runtime::BrassCoroFrame*>(frameAddr);
    frame->state_id = 1;
    frame->is_done = 0;

    // Slot 0: NaN-tagged Value
    frame->slots[0] = initialVal.rawBits();
    // Slot 1: Raw pointer to the same object
    frame->slots[1] = initialAddr;
    // Slot 2: Scalar integer (should not be altered by GC)
    frame->slots[2] = 42ULL;

    CHECK_EQ(frame->slots[0], initialVal.rawBits());
    CHECK_EQ(frame->slots[1], initialAddr);
    CHECK_EQ(frame->slots[2], 42ULL);

    // Trigger Bronze moving semi-space collection
    heap.collect();

    // Verify Slot 0 (NaN-tagged Value) was forwarded to to-space
    Value forwardedVal = Value::fromRawBits(frame->slots[0]);
    CHECK(forwardedVal.isPointer());
    uintptr_t newAddr0 = reinterpret_cast<uintptr_t>(forwardedVal.asObject<void>());
    CHECK_NE(newAddr0, initialAddr);

    // Verify Slot 1 (raw pointer) was forwarded to the same to-space address
    uintptr_t newAddr1 = static_cast<uintptr_t>(frame->slots[1]);
    CHECK_EQ(newAddr1, newAddr0);

    // Verify Slot 2 (scalar) was unchanged
    CHECK_EQ(frame->slots[2], 42ULL);

    // Verify object contents survived intact in new location
    auto* newHdr = reinterpret_cast<HeapObjectHeader*>(newAddr0);
    CHECK_EQ(newHdr->tag, static_cast<uint16_t>(Tag::Object));
    uint64_t* newPayload = reinterpret_cast<uint64_t*>(newHdr->payload());
    CHECK_EQ(newPayload[0], 0xDEADBEEFCAFEULL);

    // Verify write barrier integration
    brass::GenerationalGC genGc(64 * 1024, 32 * 1024, 128 * 1024);
    brass::brass_set_active_generational_gc(&genGc);
    reset_write_barrier_stats();

    bronze::brassCoroFrameSetSlot(frame, 3, newAddr0);
    CHECK_EQ(frame->slots[3], newAddr0);
    CHECK_GE(get_write_barrier_stats().total_invocations, 1ULL);

    brass::brass_set_active_generational_gc(nullptr);
    bronze::NativeCoroBuilder::destroyInstance(frameAddr);
}

TEST_CASE("coroutine - multi-tier consistent execution across FastInterpreter, Baseline JIT, and Optimized JIT") {
    auto buildTestModule = [](const std::string& modName) -> std::unique_ptr<brass::Module> {
        auto mod = std::make_unique<brass::Module>(modName);
        bronze::NativeCoroBuilder coroBuilder(*mod);

        brass::Function* fn = coroBuilder.createCoroFunction("fib_step", brass::Type::i64());
        brass::Builder b(*mod);
        b.set_function(fn);

        brass::BasicBlock* entry = b.append_block("entry");
        b.position_at_end(entry);
        b.add_block_param(entry, brass::Type::gcref());

        // Step 1: yield 10
        brass::Value* v1 = b.build_iconst_i64(10);
        coroBuilder.emitYield(b, v1, 1);

        // Step 2: v2 = v1 + 20 (30); yield 30
        brass::Value* v2 = b.build_add(v1, b.build_iconst_i64(20));
        coroBuilder.emitYield(b, v2, 2);

        // Step 3: v3 = v2 + 20 (50); ret 50
        brass::Value* v3 = b.build_add(v2, b.build_iconst_i64(20));
        b.build_ret(v3);

        bronze::runCoroTransformPass(*fn);
        return mod;
    };

    // 1. Tier 2: Optimized JIT
    {
        auto mod = buildTestModule("mod_opt_jit");
        brass::codegen::JitExecutionEngine jit(brass::Target::host());
        bronze::registerBrassCoroutineSymbols(jit);
        jit.compile_and_load(*mod);

        void* fnPtr = jit.get_symbol_address("fib_step");
        REQUIRE(fnPtr != nullptr);

        uintptr_t frame = bronze::NativeCoroBuilder::createInstance(fnPtr, 16, 0);
        auto s1 = bronze::NativeCoroBuilder::stepGenerator(frame);
        auto s2 = bronze::NativeCoroBuilder::stepGenerator(frame);
        auto s3 = bronze::NativeCoroBuilder::stepGenerator(frame);

        CHECK_EQ(s1.value, 10ULL);
        CHECK_FALSE(s1.done);
        CHECK_EQ(s2.value, 30ULL);
        CHECK_FALSE(s2.done);
        CHECK_EQ(s3.value, 50ULL);
        CHECK(s3.done);

        bronze::NativeCoroBuilder::destroyInstance(frame);
    }

    // 2. Tier 1: Baseline JIT
    {
        auto mod = buildTestModule("mod_baseline");
        brass::codegen::BaselineJitCompiler compiler(brass::Target::host());
        bronze::registerBrassCoroutineSymbols(compiler);

        auto compiled = compiler.compile_module(*mod);
        void* fnPtr = nullptr;
        for (auto& cf : compiled) {
            if (cf.name() == "fib_step") {
                fnPtr = cf.entry_point();
                break;
            }
        }
        REQUIRE(fnPtr != nullptr);

        uintptr_t frame = bronze::NativeCoroBuilder::createInstance(fnPtr, 16, 0);
        auto s1 = bronze::NativeCoroBuilder::stepGenerator(frame);
        auto s2 = bronze::NativeCoroBuilder::stepGenerator(frame);
        auto s3 = bronze::NativeCoroBuilder::stepGenerator(frame);

        CHECK_EQ(s1.value, 10ULL);
        CHECK_FALSE(s1.done);
        CHECK_EQ(s2.value, 30ULL);
        CHECK_FALSE(s2.done);
        CHECK_EQ(s3.value, 50ULL);
        CHECK(s3.done);

        bronze::NativeCoroBuilder::destroyInstance(frame);
    }

    // 3. Tier 0: FastInterpreter
    {
        auto mod = buildTestModule("mod_interp");
        brass::FastInterpreter interp(64 * 1024);
        bronze::registerBrassCoroutineSymbols(interp);

        brass::Function* t_fn = mod->get_function("fib_step");
        REQUIRE(t_fn != nullptr);

        uintptr_t c_frame = bronze::NativeCoroBuilder::createInstance(nullptr, 16, 0);
        REQUIRE(c_frame != 0);
        auto* f_ptr = reinterpret_cast<brass::runtime::BrassCoroFrame*>(c_frame);

        // Step 1
        brass::RuntimeValue o1 = interp.run(*t_fn, {brass::RuntimeValue::from_ptr(c_frame)});
        CHECK_EQ(o1.as_u64(), 10ULL);
        CHECK_EQ(f_ptr->is_done, 0u);

        // Step 2
        brass::RuntimeValue o2 = interp.run(*t_fn, {brass::RuntimeValue::from_ptr(c_frame)});
        CHECK_EQ(o2.as_u64(), 30ULL);
        CHECK_EQ(f_ptr->is_done, 0u);

        // Step 3
        brass::RuntimeValue o3 = interp.run(*t_fn, {brass::RuntimeValue::from_ptr(c_frame)});
        CHECK_EQ(o3.as_u64(), 50ULL);
        CHECK_EQ(f_ptr->is_done, 1u);

        bronze::NativeCoroBuilder::destroyInstance(c_frame);
    }
}

TEST_CASE("coroutine - NativeCoroLowerer generator and async bridge") {
    brass::Module mod("coro_lowerer_test");
    bronze::NativeCoroLowerer lowerer;
    CHECK_EQ(lowerer.mode(), bronze::CoroutineLoweringMode::NativeCoroMIR);

    brass::Function* genFn = lowerer.lowerGeneratorFunction(
        mod, "test_gen",
        [](brass::Builder& b, bronze::NativeCoroBuilder& coro) {
            brass::Value* val = b.build_iconst_i64(777);
            coro.emitYield(b, val, 1);
            b.build_ret(val);
        });
    REQUIRE(genFn != nullptr);

    brass::Function* asyncFn = lowerer.lowerAsyncFunction(
        mod, "test_async",
        [](brass::Builder& b, bronze::NativeCoroBuilder& coro) {
            brass::Value* val = b.build_iconst_i64(888);
            coro.emitAwait(b, val, 1);
            b.build_ret(val);
        });
    REQUIRE(asyncFn != nullptr);

    bool changed = bronze::transformCoroutinesIfNeeded(mod);
    CHECK(changed);
}
