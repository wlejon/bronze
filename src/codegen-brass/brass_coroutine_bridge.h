#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <brass/codegen/baseline_jit.hpp>
#include <brass/codegen/jit_exec.hpp>
#include <brass/vm/fast_interpreter.hpp>
#include <brass/mir/builder.hpp>
#include <brass/mir/coro_transform.hpp>
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>
#include <brass/runtime/coroutine.hpp>
#include <brass/runtime/multi_tier_pipeline.hpp>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace bronze {

// ============================================================================
// 1. Symbol Registration Helpers for Brass Coroutine Runtime
// ============================================================================

// Registers Brass coroutine runtime functions:
//   - brass_coro_create
//   - brass_coro_resume
//   - brass_coro_is_done
//   - brass_coro_destroy
//   - bronze_iter_open
//   - bronze_iter_step
//   - bronze_create_async_machine
//   - bronze_async_start
//   - bronze_async_await
// across JIT, baseline, fast interpreter, and multi-tier execution engines.
void registerBrassCoroutineSymbols(brass::FastInterpreter& interp);
void registerBrassCoroutineSymbols(brass::codegen::BaselineJitCompiler& compiler);
void registerBrassCoroutineSymbols(brass::codegen::JitExecutionEngine& engine);
void registerBrassCoroutineSymbols(brass::runtime::MultiTierPipeline& pipeline);

// ============================================================================
// 2. BrassCoroFrame GC Root Tracing & Write Barrier Alignment
// ============================================================================

// Visits all active, non-completed coroutine frames and forwards any live
// Bronze heap references (both NaN-tagged Values and raw semispace addresses)
// held in yielded_val, resume_arg, and spilled frame slots.
void traceActiveCoroFrameRoots(Heap& heap, const Heap::RootVisitor& visit);

// Registers an active coroutine GC root source with Bronze's moving Cheney collector.
void registerCoroFrameGcRootSource(Heap& heap);

// Invokes the GC write barrier when storing into a BrassCoroFrame slot,
// ensuring card table marking if the frame is tenured and the value is young.
void brassCoroFrameWriteBarrier(brass::runtime::BrassCoroFrame* frame, uint64_t val);

// Writes a slot in a BrassCoroFrame and fires the generational GC write barrier.
void brassCoroFrameSetSlot(brass::runtime::BrassCoroFrame* frame, uint32_t slotIdx, uint64_t val);

// ============================================================================
// 3. Coroutine Transformation Pass Helpers
// ============================================================================

// Checks whether a function or module contains native coroutine suspend/resume opcodes.
bool functionContainsCoroOpcodes(const brass::Function& fn) noexcept;
bool moduleContainsCoroOpcodes(const brass::Module& mod) noexcept;

// Runs CoroTransformPass on a function or module, converting coro_suspend
// points into stackless resumable state machines with spilled SSA frame slots.
bool runCoroTransformPass(brass::Function& fn, const brass::CoroTransformOptions& options = {});
bool runCoroTransformPass(brass::Module& mod, const brass::CoroTransformOptions& options = {});

// Automatically detects coroutine suspend points in a module and runs
// CoroTransformPass if needed.
bool transformCoroutinesIfNeeded(brass::Module& mod, const brass::CoroTransformOptions& options = {});

// ============================================================================
// 4. Native Coroutine MIR Lowerer & Pipeline Bridge
// ============================================================================

enum class CoroutineLoweringMode {
    FrontendStateMachine, // Standard frontend lowering (manual switch & env frames)
    NativeCoroMIR         // Brass first-class MIR coroutines (coro_create/suspend/resume)
};

struct CoroGeneratorResult {
    uint64_t value = 0;
    bool done = false;
};

// Builder bridge providing first-class MIR construction and lowering of
// coroutines, generators, and async functions.
class NativeCoroBuilder {
public:
    explicit NativeCoroBuilder(brass::Module& mod);

    brass::Module& module() noexcept { return mod_; }

    // Creates a function prototype configured for coroutine state machine transformation.
    // By convention, the state machine takes a BrassCoroFrame pointer as its first parameter.
    brass::Function* createCoroFunction(std::string_view name,
                                        brass::Type returnType = brass::Type::i64(),
                                        const std::vector<brass::Type>& extraParams = {});

    // Emits a coro_suspend instruction inside the current basic block.
    // Returns the SSA value containing the resume argument upon resumption.
    brass::Value* emitYield(brass::Builder& b, brass::Value* yieldVal, uint32_t resumeId = 0);

    // Emits an await suspend point inside an async function.
    brass::Value* emitAwait(brass::Builder& b, brass::Value* promiseVal, uint32_t resumeId = 0);

    // Transforms all coroutines in the module into stackless resumable state machines.
    bool transform(const brass::CoroTransformOptions& options = {});

    // Instantiates a coroutine frame with the given function pointer and slot count.
    static uintptr_t createInstance(void* fnPtr, uint32_t slotCount = 16, uint64_t pointerMask = 0);

    // Resumes a coroutine frame with an input value.
    static uint64_t resumeInstance(uintptr_t frameAddr, uint64_t inputVal = 0);

    // Checks if the coroutine has completed execution.
    static bool isDone(uintptr_t frameAddr);

    // Destroys and unregisters a coroutine frame.
    static void destroyInstance(uintptr_t frameAddr);

    // High-level generator stepping helper returning IteratorResult { value, done }.
    static CoroGeneratorResult stepGenerator(uintptr_t frameAddr, uint64_t sentVal = 0);

private:
    brass::Module& mod_;
    uint32_t nextResumeId_ = 1;
};

// Selectable pipeline lowerer that lowers high-level generator and async operations
// to native Brass coroutine MIR.
class NativeCoroLowerer {
public:
    NativeCoroLowerer() = default;

    // Configures the active coroutine lowering mode.
    void setMode(CoroutineLoweringMode mode) noexcept { mode_ = mode; }
    CoroutineLoweringMode mode() const noexcept { return mode_; }

    // Generates a self-contained generator state machine function in MIR with
    // native coro_suspend points for each yielded value expression.
    brass::Function* lowerGeneratorFunction(
        brass::Module& mod,
        std::string_view name,
        const std::function<void(brass::Builder&, NativeCoroBuilder&)>& bodyEmitter);

    // Generates an async state machine function in MIR with native
    // coro_suspend points for each await expression.
    brass::Function* lowerAsyncFunction(
        brass::Module& mod,
        std::string_view name,
        const std::function<void(brass::Builder&, NativeCoroBuilder&)>& bodyEmitter);

private:
    CoroutineLoweringMode mode_ = CoroutineLoweringMode::NativeCoroMIR;
};

} // namespace bronze
