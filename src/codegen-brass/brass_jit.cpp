#include "codegen-brass/brass_jit.h"
#include "codegen-brass/brass_backend.h"

#include "abi/bronze_abi.h"

#include <brass/codegen/jit_exec.hpp>
#include <brass/il_translator/il_translator.hpp>
#include <brass/target/target.hpp>

namespace bronze {

BrassJitProgram::BrassJitProgram(std::unique_ptr<brass::codegen::JitExecutionEngine> engine,
                                 void* entryPoint, const void* codeRanges, uint32_t codeRangeCount)
    : engine_(std::move(engine)), entryPoint_(entryPoint), codeRanges_(codeRanges), codeRangeCount_(codeRangeCount) {}

BrassJitProgram::~BrassJitProgram() {
    if (codeRanges_ && codeRangeCount_ > 0) {
        bronze_unregister_code_ranges(codeRanges_, codeRangeCount_);
    }
}

BrassJitProgram::BrassJitProgram(BrassJitProgram&&) noexcept = default;
BrassJitProgram& BrassJitProgram::operator=(BrassJitProgram&&) noexcept = default;

void* BrassJitProgram::entryPoint() const noexcept {
    return entryPoint_;
}

void* BrassJitProgram::symbolAddress(std::string_view name) const {
    if (!engine_) {
        return nullptr;
    }
    return engine_->get_symbol_address(name);
}

void BrassJitProgram::run() {
    if (entryPoint_) {
        auto fn = reinterpret_cast<void (*)()>(entryPoint_);
        fn();
    }
}

std::unique_ptr<BrassJitProgram> BrassBackend::compileToJit(const il::Module& module,
                                                            DiagnosticSink& diags) {
    const bool prevPropagate = propagateExceptionsInEntry_;
    propagateExceptionsInEntry_ = true;
    auto obj = buildObjectFile(module, diags);
    propagateExceptionsInEntry_ = prevPropagate;
    if (!obj) {
        return nullptr;
    }

    auto engine = std::make_unique<brass::codegen::JitExecutionEngine>(brass::Target::host());
    brass::il::register_bronze_runtime_symbols(engine.get());

#define BRONZE_ABI_REG_JIT(name, ret, args) \
    engine->register_external_symbol(#name, reinterpret_cast<void*>(&::name));
    BRONZE_ABI_FUNCTIONS(BRONZE_ABI_REG_JIT)
#undef BRONZE_ABI_REG_JIT

    if (!engine->load_object(*obj)) {
        diags.error(Span{}, "Failed to load object into JIT execution engine");
        return nullptr;
    }

    void* entry = engine->get_symbol_address(entrySymbol_);
    std::string rangesSym = (entrySymbol_ == "bronze_main") ? "bronze_object_code_ranges" : (entrySymbol_ + "_code_ranges");
    std::string countSym = (entrySymbol_ == "bronze_main") ? "bronze_object_code_range_count" : (entrySymbol_ + "_code_range_count");
    void* rangesAddr = engine->get_symbol_address(rangesSym);
    void* countAddr = engine->get_symbol_address(countSym);
    uint32_t count = countAddr ? *reinterpret_cast<const uint32_t*>(countAddr) : 0;
    if (rangesAddr && count > 0) {
        bronze_register_code_ranges(rangesAddr, count);
    }
    return std::make_unique<BrassJitProgram>(std::move(engine), entry, rangesAddr, count);
}

}  // namespace bronze
