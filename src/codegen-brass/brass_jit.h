#pragma once

#include <memory>
#include <string_view>

namespace brass::codegen {
class JitExecutionEngine;
}

namespace bronze {

class BrassJitProgram {
public:
    BrassJitProgram(std::unique_ptr<brass::codegen::JitExecutionEngine> engine, void* entryPoint);
    ~BrassJitProgram();

    BrassJitProgram(const BrassJitProgram&) = delete;
    BrassJitProgram& operator=(const BrassJitProgram&) = delete;

    BrassJitProgram(BrassJitProgram&&) noexcept;
    BrassJitProgram& operator=(BrassJitProgram&&) noexcept;

    void* entryPoint() const noexcept;
    void* symbolAddress(std::string_view name) const;
    void run();

private:
    std::unique_ptr<brass::codegen::JitExecutionEngine> engine_;
    void* entryPoint_ = nullptr;
};

}  // namespace bronze
