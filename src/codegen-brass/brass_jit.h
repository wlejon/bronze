#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace brass {
class ModuleStackMap;
}

namespace brass::codegen {
class JitExecutionEngine;
}

namespace bronze {

class BrassJitProgram {
public:
    BrassJitProgram(std::unique_ptr<brass::codegen::JitExecutionEngine> engine, void* entryPoint,
                    const void* codeRanges = nullptr, uint32_t codeRangeCount = 0,
                    std::vector<const void*> codeTokens = {});
    ~BrassJitProgram();

    BrassJitProgram(const BrassJitProgram&) = delete;
    BrassJitProgram& operator=(const BrassJitProgram&) = delete;

    BrassJitProgram(BrassJitProgram&&) noexcept;
    BrassJitProgram& operator=(BrassJitProgram&&) noexcept;

    void* entryPoint() const noexcept;
    void* symbolAddress(std::string_view name) const;
    brass::codegen::JitExecutionEngine* engine() const noexcept { return engine_.get(); }
    const brass::ModuleStackMap* stackMaps() const noexcept;
    const std::vector<const void*>& codeTokens() const noexcept { return codeTokens_; }
    void run();

private:
    std::unique_ptr<brass::codegen::JitExecutionEngine> engine_;
    void* entryPoint_ = nullptr;
    const void* codeRanges_ = nullptr;
    uint32_t codeRangeCount_ = 0;
    std::vector<const void*> codeTokens_;
};

}  // namespace bronze
