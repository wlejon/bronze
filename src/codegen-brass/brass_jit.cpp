#include "codegen-brass/brass_jit.h"
#include "codegen-brass/brass_backend.h"
#include "codegen-brass/brass_symbol_registration.h"

#include "abi/bronze_abi.h"

#include <brass/codegen/jit_exec.hpp>
#include <brass/gc/runtime_gc.hpp>
#include <brass/target/target.hpp>

#include <utility>

namespace bronze {

BrassJitProgram::BrassJitProgram(std::unique_ptr<brass::codegen::JitExecutionEngine> engine,
                                 void* entryPoint, const void* codeRanges, uint32_t codeRangeCount)
    : engine_(std::move(engine)),
      entryPoint_(entryPoint),
      codeRanges_(codeRanges),
      codeRangeCount_(codeRangeCount) {
    if (engine_) {
        brass::brass_set_active_stack_maps(&engine_->stack_maps());
    }
}

BrassJitProgram::~BrassJitProgram() {
    if (engine_ && brass::brass_get_active_stack_maps() == &engine_->stack_maps()) {
        brass::brass_set_active_stack_maps(nullptr);
    }
    if (codeRanges_ && codeRangeCount_ > 0) {
        bronze_unregister_code_ranges(codeRanges_, codeRangeCount_);
        codeRanges_ = nullptr;
        codeRangeCount_ = 0;
    }
}

BrassJitProgram::BrassJitProgram(BrassJitProgram&& other) noexcept
    : engine_(std::move(other.engine_)),
      entryPoint_(std::exchange(other.entryPoint_, nullptr)),
      codeRanges_(std::exchange(other.codeRanges_, nullptr)),
      codeRangeCount_(std::exchange(other.codeRangeCount_, 0)) {
    if (engine_) {
        brass::brass_set_active_stack_maps(&engine_->stack_maps());
    }
}

BrassJitProgram& BrassJitProgram::operator=(BrassJitProgram&& other) noexcept {
    if (this != &other) {
        if (engine_ && brass::brass_get_active_stack_maps() == &engine_->stack_maps()) {
            brass::brass_set_active_stack_maps(nullptr);
        }
        if (codeRanges_ && codeRangeCount_ > 0) {
            bronze_unregister_code_ranges(codeRanges_, codeRangeCount_);
        }
        engine_ = std::move(other.engine_);
        entryPoint_ = std::exchange(other.entryPoint_, nullptr);
        codeRanges_ = std::exchange(other.codeRanges_, nullptr);
        codeRangeCount_ = std::exchange(other.codeRangeCount_, 0);
        if (engine_) {
            brass::brass_set_active_stack_maps(&engine_->stack_maps());
        }
    }
    return *this;
}

void* BrassJitProgram::entryPoint() const noexcept {
    return entryPoint_;
}

void* BrassJitProgram::symbolAddress(std::string_view name) const {
    if (!engine_) {
        return nullptr;
    }
    return engine_->get_symbol_address(name);
}

const brass::ModuleStackMap* BrassJitProgram::stackMaps() const noexcept {
    return engine_ ? &engine_->stack_maps() : nullptr;
}

void BrassJitProgram::run() {
    if (engine_) {
        brass::brass_set_active_stack_maps(&engine_->stack_maps());
    }
    if (entryPoint_) {
        auto fn = reinterpret_cast<void (*)()>(entryPoint_);
        fn();
    }
}

std::unique_ptr<BrassJitProgram> BrassBackend::compileToJit(const il::Module& module,
                                                            DiagnosticSink& diags) {
    struct PropagateGuard {
        bool& flag;
        bool prev;
        PropagateGuard(bool& f, bool val) : flag(f), prev(f) { flag = val; }
        ~PropagateGuard() { flag = prev; }
    } guard(propagateExceptionsInEntry_, true);
    // A JIT program is compiled and run on one thread (eval, new Function,
    // a host's evalScript), and there are many of them: per-thread instances
    // would buy it nothing and keep each one's pristine snapshot alive for
    // the life of the process (runtime/module_instance.cpp). Only an image
    // loaded once and entered on N threads needs them.
    PropagateGuard perThreadGuard(perThreadModuleData_, false);

    auto obj = buildObjectFile(module, diags);
    if (!obj) {
        return nullptr;
    }

    auto engine = std::make_unique<brass::codegen::JitExecutionEngine>(brass::Target::host());
    registerBronzeJitSymbols(*engine);

    for (const auto& fn : module.functions) {
        std::vector<brass::Type> paramTypes;
        paramTypes.reserve(fn.params.size());
        for (const auto& p : fn.params) {
            paramTypes.push_back(brassTypeOf(p.type));
        }
        engine->register_function_signature(fn.name, brassTypeOf(fn.returnType), std::move(paramTypes));
    }

    if (!engine->load_object(*obj)) {
        diags.error(Span{}, "Failed to load object into JIT execution engine");
        return nullptr;
    }

    void* entry = engine->get_symbol_address(entrySymbol_);
    const std::string rangesSym = (entrySymbol_ == "bronze_main")
                                      ? "bronze_object_code_ranges"
                                      : (entrySymbol_ + "_code_ranges");
    const std::string countSym = (entrySymbol_ == "bronze_main")
                                     ? "bronze_object_code_range_count"
                                     : (entrySymbol_ + "_code_range_count");
    void* rangesAddr = engine->get_symbol_address(rangesSym);
    void* countAddr = engine->get_symbol_address(countSym);
    const uint32_t count = countAddr ? *reinterpret_cast<const uint32_t*>(countAddr) : 0;
    if (rangesAddr && count > 0) {
        bronze_register_code_ranges(rangesAddr, count);
    }
    return std::make_unique<BrassJitProgram>(std::move(engine), entry, rangesAddr, count);
}

}  // namespace bronze
