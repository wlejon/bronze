#include "codegen-brass/brass_jit.h"
#include "codegen-brass/brass_backend.h"

#include "abi/bronze_abi.h"

#include <brass/codegen/jit_exec.hpp>
#include <brass/il_translator/il_translator.hpp>
#include <brass/target/target.hpp>

#include <utility>

namespace bronze {

BrassJitProgram::BrassJitProgram(std::unique_ptr<brass::codegen::JitExecutionEngine> engine,
                                 void* entryPoint, const void* codeRanges, uint32_t codeRangeCount)
    : engine_(std::move(engine)),
      entryPoint_(entryPoint),
      codeRanges_(codeRanges),
      codeRangeCount_(codeRangeCount) {}

BrassJitProgram::~BrassJitProgram() {
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
      codeRangeCount_(std::exchange(other.codeRangeCount_, 0)) {}

BrassJitProgram& BrassJitProgram::operator=(BrassJitProgram&& other) noexcept {
    if (this != &other) {
        if (codeRanges_ && codeRangeCount_ > 0) {
            bronze_unregister_code_ranges(codeRanges_, codeRangeCount_);
        }
        engine_ = std::move(other.engine_);
        entryPoint_ = std::exchange(other.entryPoint_, nullptr);
        codeRanges_ = std::exchange(other.codeRanges_, nullptr);
        codeRangeCount_ = std::exchange(other.codeRangeCount_, 0);
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

void BrassJitProgram::run() {
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

    auto obj = buildObjectFile(module, diags);
    if (!obj) {
        return nullptr;
    }

    auto engine = std::make_unique<brass::codegen::JitExecutionEngine>(brass::Target::host());
    brass::il::register_bronze_runtime_symbols(engine.get());

#define BRONZE_ABI_REG_JIT(name, ret, args) \
    engine->register_external_symbol(#name, reinterpret_cast<void*>(&::name));
    BRONZE_ABI_FUNCTIONS(BRONZE_ABI_REG_JIT)
#undef BRONZE_ABI_REG_JIT

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
