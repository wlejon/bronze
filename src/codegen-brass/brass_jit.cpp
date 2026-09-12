#include "codegen-brass/brass_jit.h"

#include <brass/codegen/jit_exec.hpp>

namespace bronze {

BrassJitProgram::BrassJitProgram(std::unique_ptr<brass::codegen::JitExecutionEngine> engine,
                                 void* entryPoint)
    : engine_(std::move(engine)), entryPoint_(entryPoint) {}

BrassJitProgram::~BrassJitProgram() = default;

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

}  // namespace bronze
