#include "codegen-brass/brass_coroutine_bridge.h"

#include <algorithm>
#include <cstring>
#include <type_traits>
#include <utility>

#include <brass/gc/runtime_gc.hpp>
#include <brass/mir/opcodes.hpp>
#include <brass/mir/verifier.hpp>

namespace bronze {

namespace {

// A raw interpreter slot becomes either a pointer or an integer argument;
// reinterpret_cast only covers the pointer case (MSVC rejects it int-to-int).
template <typename T>
T fromSlot(uint64_t raw) {
    if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<T>(static_cast<uintptr_t>(raw));
    } else {
        return static_cast<T>(raw);
    }
}

// How a host function's result is boxed. uintptr_t and uint64_t are the same
// type on LP64 Linux and on Windows but distinct on macOS, so a pointer-sized
// handle is named at the call site rather than inferred from the type.
enum class HostRet { Auto, Handle };

template <HostRet As, typename Ret, typename... Args, size_t... I>
brass::RuntimeValue invokeFromSlots(Ret (*fn)(Args...),
                                    const std::vector<brass::RuntimeValue>& args,
                                    std::index_sequence<I...>) {
    auto slot = [&args](size_t idx) -> uint64_t {
        return idx < args.size() ? args[idx].as_u64() : 0;
    };
    (void)slot;  // unused when the function takes no arguments
    if constexpr (std::is_void_v<Ret>) {
        fn(fromSlot<Args>(slot(I))...);
        return brass::RuntimeValue::from_void();
    } else if constexpr (As == HostRet::Handle) {
        return brass::RuntimeValue::from_ptr(static_cast<uintptr_t>(fn(fromSlot<Args>(slot(I))...)));
    } else if constexpr (sizeof(Ret) <= sizeof(uint32_t)) {
        return brass::RuntimeValue::from_i32(static_cast<int32_t>(fn(fromSlot<Args>(slot(I))...)));
    } else {
        return brass::RuntimeValue::from_u64(static_cast<uint64_t>(fn(fromSlot<Args>(slot(I))...)));
    }
}

template <HostRet As = HostRet::Auto, typename Ret, typename... Args>
brass::FastHostFn makeFastHostFn(Ret (*fn)(Args...)) {
    return [fn](brass::FastInterpreter&, const std::vector<brass::RuntimeValue>& args) -> brass::RuntimeValue {
        return invokeFromSlots<As>(fn, args, std::index_sequence_for<Args...>{});
    };
}

static inline void traceSingleWord(Heap& heap, const Heap::RootVisitor& visit, uint64_t& word) {
    if (word == 0) return;

    // 1. NaN-tagged bronze::Value
    Value val = Value::fromRawBits(word);
    if (val.isPointer()) {
        visit(val);
        word = val.rawBits();
        return;
    }

    // 2. Raw 48-bit heap address in user space pointing into semispace
    const uint64_t high16 = word >> kTagShift;
    if (high16 == 0) {
        const auto* raw = reinterpret_cast<const uint8_t*>(word);
        if (raw >= heap.from_space().base && raw < heap.from_space().bump_ptr) {
            Value tmp = Value::fromObject(raw);
            visit(tmp);
            word = reinterpret_cast<uintptr_t>(tmp.asObject<void>());
        }
    }
}

} // namespace

// ============================================================================
// 1. Symbol Registration Helpers
// ============================================================================

void registerBrassCoroutineSymbols(brass::FastInterpreter& interp) {
    // 1. Coroutine ABI functions
    interp.register_external_symbol("brass_coro_create", reinterpret_cast<void*>(&::brass_coro_create));
    interp.register_external_function("brass_coro_create", makeFastHostFn<HostRet::Handle>(&::brass_coro_create));

    interp.register_external_symbol("brass_coro_resume", reinterpret_cast<void*>(&::brass_coro_resume));
    interp.register_external_function("brass_coro_resume", makeFastHostFn(&::brass_coro_resume));

    interp.register_external_symbol("brass_coro_is_done", reinterpret_cast<void*>(&::brass_coro_is_done));
    interp.register_external_function("brass_coro_is_done", makeFastHostFn(&::brass_coro_is_done));

    interp.register_external_symbol("brass_coro_destroy", reinterpret_cast<void*>(&::brass_coro_destroy));
    interp.register_external_function("brass_coro_destroy", makeFastHostFn(&::brass_coro_destroy));

    // 2. Coroutine-based Async Helper
    interp.register_external_symbol("bronze_create_async_machine", reinterpret_cast<void*>(&brass::runtime::bronze_create_async_machine));
    interp.register_external_function("bronze_create_async_machine", makeFastHostFn(&brass::runtime::bronze_create_async_machine));
}

void registerBrassCoroutineSymbols(brass::codegen::BaselineJitCompiler& compiler) {
    compiler.register_external_symbol("brass_coro_create", reinterpret_cast<void*>(&::brass_coro_create));
    compiler.register_external_symbol("brass_coro_resume", reinterpret_cast<void*>(&::brass_coro_resume));
    compiler.register_external_symbol("brass_coro_is_done", reinterpret_cast<void*>(&::brass_coro_is_done));
    compiler.register_external_symbol("brass_coro_destroy", reinterpret_cast<void*>(&::brass_coro_destroy));

    compiler.register_external_symbol("bronze_create_async_machine", reinterpret_cast<void*>(&brass::runtime::bronze_create_async_machine));
}

void registerBrassCoroutineSymbols(brass::codegen::JitExecutionEngine& engine) {
    engine.register_external_symbol("brass_coro_create", reinterpret_cast<void*>(&::brass_coro_create));
    engine.register_external_symbol("brass_coro_resume", reinterpret_cast<void*>(&::brass_coro_resume));
    engine.register_external_symbol("brass_coro_is_done", reinterpret_cast<void*>(&::brass_coro_is_done));
    engine.register_external_symbol("brass_coro_destroy", reinterpret_cast<void*>(&::brass_coro_destroy));

    engine.register_external_symbol("bronze_create_async_machine", reinterpret_cast<void*>(&brass::runtime::bronze_create_async_machine));
}

void registerBrassCoroutineSymbols(brass::runtime::MultiTierPipeline& pipeline) {
    pipeline.register_external_symbol("brass_coro_create", reinterpret_cast<void*>(&::brass_coro_create));
    pipeline.register_external_function("brass_coro_create", makeFastHostFn<HostRet::Handle>(&::brass_coro_create));

    pipeline.register_external_symbol("brass_coro_resume", reinterpret_cast<void*>(&::brass_coro_resume));
    pipeline.register_external_function("brass_coro_resume", makeFastHostFn(&::brass_coro_resume));

    pipeline.register_external_symbol("brass_coro_is_done", reinterpret_cast<void*>(&::brass_coro_is_done));
    pipeline.register_external_function("brass_coro_is_done", makeFastHostFn(&::brass_coro_is_done));

    pipeline.register_external_symbol("brass_coro_destroy", reinterpret_cast<void*>(&::brass_coro_destroy));
    pipeline.register_external_function("brass_coro_destroy", makeFastHostFn(&::brass_coro_destroy));

    pipeline.register_external_symbol("bronze_create_async_machine", reinterpret_cast<void*>(&brass::runtime::bronze_create_async_machine));
    pipeline.register_external_function("bronze_create_async_machine", makeFastHostFn(&brass::runtime::bronze_create_async_machine));
}

// ============================================================================
// 2. BrassCoroFrame GC Root Tracing & Write Barrier Alignment
// ============================================================================

void traceActiveCoroFrameRoots(Heap& heap, const Heap::RootVisitor& visit) {
    brass::runtime::visit_active_coro_frames([&heap, &visit](uintptr_t* frame_ptr) {
        if (!frame_ptr || !*frame_ptr) return;
        auto* frame = reinterpret_cast<brass::runtime::BrassCoroFrame*>(*frame_ptr);
        if (!frame || frame->is_done) return;

        // 1. Trace last yielded value
        traceSingleWord(heap, visit, frame->yielded_val);

        // 2. Trace resumption argument
        traceSingleWord(heap, visit, frame->resume_arg);

        // 3. Trace all spilled SSA slots
        for (uint32_t i = 0; i < frame->slot_count; ++i) {
            traceSingleWord(heap, visit, frame->slots[i]);
        }
    });
}

void registerCoroFrameGcRootSource(Heap& heap) {
    heap.add_root_source([&heap](const Heap::RootVisitor& visit) {
        traceActiveCoroFrameRoots(heap, visit);
    });
}

void brassCoroFrameWriteBarrier(brass::runtime::BrassCoroFrame* frame, uint64_t val) {
    if (!frame) return;
    brass_gc_write_barrier(reinterpret_cast<uint64_t>(frame), val);
}

void brassCoroFrameSetSlot(brass::runtime::BrassCoroFrame* frame, uint32_t slotIdx, uint64_t val) {
    if (!frame) return;
    frame->slots[slotIdx] = val;
    brassCoroFrameWriteBarrier(frame, val);
}

// ============================================================================
// 3. Coroutine Transformation Pass Helpers
// ============================================================================

bool functionContainsCoroOpcodes(const brass::Function& fn) noexcept {
    for (const auto* bb : fn.blocks()) {
        for (const auto* inst : *bb) {
            if (inst->opcode() == brass::Opcode::coro_suspend ||
                inst->opcode() == brass::Opcode::coro_create ||
                inst->opcode() == brass::Opcode::coro_resume ||
                inst->opcode() == brass::Opcode::coro_destroy) {
                return true;
            }
        }
    }
    return false;
}

bool moduleContainsCoroOpcodes(const brass::Module& mod) noexcept {
    for (const auto& fn : mod.functions()) {
        if (functionContainsCoroOpcodes(*fn)) return true;
    }
    return false;
}

bool runCoroTransformPass(brass::Function& fn, const brass::CoroTransformOptions& options) {
    brass::CoroTransformPass pass(options);
    return pass.run_on_function(fn);
}

bool runCoroTransformPass(brass::Module& mod, const brass::CoroTransformOptions& options) {
    brass::CoroTransformPass pass(options);
    return pass.run_on_module(mod);
}

bool transformCoroutinesIfNeeded(brass::Module& mod, const brass::CoroTransformOptions& options) {
    if (!moduleContainsCoroOpcodes(mod)) return false;
    return runCoroTransformPass(mod, options);
}

// ============================================================================
// 4. Native Coroutine MIR Lowerer & Builder
// ============================================================================

NativeCoroBuilder::NativeCoroBuilder(brass::Module& mod) : mod_(mod) {}

brass::Function* NativeCoroBuilder::createCoroFunction(
    std::string_view name,
    brass::Type returnType,
    const std::vector<brass::Type>& extraParams) {
    std::vector<brass::Type> paramTypes = {brass::Type::gcref()};
    paramTypes.insert(paramTypes.end(), extraParams.begin(), extraParams.end());
    return mod_.create_function(name, returnType, std::move(paramTypes));
}

brass::Value* NativeCoroBuilder::emitYield(brass::Builder& b, brass::Value* yieldVal, uint32_t resumeId) {
    uint32_t rid = (resumeId != 0) ? resumeId : (nextResumeId_++);
    return b.build_coro_suspend(yieldVal, rid, brass::Type::i64());
}

brass::Value* NativeCoroBuilder::emitAwait(brass::Builder& b, brass::Value* promiseVal, uint32_t resumeId) {
    uint32_t rid = (resumeId != 0) ? resumeId : (nextResumeId_++);
    return b.build_coro_suspend(promiseVal, rid, brass::Type::i64());
}

bool NativeCoroBuilder::transform(const brass::CoroTransformOptions& options) {
    brass::CoroTransformPass pass(options);
    return pass.run_on_module(mod_);
}

uintptr_t NativeCoroBuilder::createInstance(void* fnPtr, uint32_t slotCount, uint64_t pointerMask) {
    return brass_coro_create(fnPtr, slotCount, pointerMask);
}

uint64_t NativeCoroBuilder::resumeInstance(uintptr_t frameAddr, uint64_t inputVal) {
    return brass_coro_resume(frameAddr, inputVal);
}

bool NativeCoroBuilder::isDone(uintptr_t frameAddr) {
    return brass_coro_is_done(frameAddr) != 0;
}

void NativeCoroBuilder::destroyInstance(uintptr_t frameAddr) {
    brass_coro_destroy(frameAddr);
}

CoroGeneratorResult NativeCoroBuilder::stepGenerator(uintptr_t frameAddr, uint64_t sentVal) {
    if (!frameAddr) return {0, true};
    uint64_t res = resumeInstance(frameAddr, sentVal);
    bool done = isDone(frameAddr);
    return {res, done};
}

brass::Function* NativeCoroLowerer::lowerGeneratorFunction(
    brass::Module& mod,
    std::string_view name,
    const std::function<void(brass::Builder&, NativeCoroBuilder&)>& bodyEmitter) {
    NativeCoroBuilder coroBuilder(mod);
    brass::Function* fn = coroBuilder.createCoroFunction(name, brass::Type::i64());

    brass::Builder b(mod);
    b.set_function(fn);
    brass::BasicBlock* entry = b.append_block("entry");
    b.position_at_end(entry);
    b.add_block_param(entry, brass::Type::gcref());

    if (bodyEmitter) {
        bodyEmitter(b, coroBuilder);
    }

    return fn;
}

brass::Function* NativeCoroLowerer::lowerAsyncFunction(
    brass::Module& mod,
    std::string_view name,
    const std::function<void(brass::Builder&, NativeCoroBuilder&)>& bodyEmitter) {
    NativeCoroBuilder coroBuilder(mod);
    brass::Function* fn = coroBuilder.createCoroFunction(name, brass::Type::i64());

    brass::Builder b(mod);
    b.set_function(fn);
    brass::BasicBlock* entry = b.append_block("entry");
    b.position_at_end(entry);
    b.add_block_param(entry, brass::Type::gcref());

    if (bodyEmitter) {
        bodyEmitter(b, coroBuilder);
    }

    return fn;
}

} // namespace bronze
