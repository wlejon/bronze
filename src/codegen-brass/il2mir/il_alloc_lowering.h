#pragma once

#include <brass/mir/builder.hpp>
#include <brass/mir/instruction.hpp>
#include <cstdint>
#include "il_ast.h"

namespace il2mir {

// Object, array and environment allocation: an inline bump in the thread's
// allocation window (bronze_abi_tls.h) with the runtime helper as the slow
// path, or the helper alone when the TLAB fast path is off.
class AllocLoweringHelper {
public:
    explicit AllocLoweringHelper(bool enable_tlab = true) : enable_tlab_(enable_tlab) {}

    [[nodiscard]] bool enable_tlab() const noexcept { return enable_tlab_; }
    void set_enable_tlab(bool enable) noexcept { enable_tlab_ = enable; }

    Value* lower_create_object(Builder& b);
    Value* lower_create_array(Builder& b, Value* size_val, uint32_t param_count);
    Value* lower_env_create(Builder& b, Value* parent_val, Value* size_val, uint32_t param_count);

private:
    Value* lower_create_object_bronze(Builder& b);
    Value* lower_create_array_bronze(Builder& b, Value* size_val, uint32_t param_count);
    Value* lower_env_create_bronze(Builder& b, Value* parent_val, Value* size_val, uint32_t param_count);

    bool enable_tlab_ = true;
};

} // namespace il2mir
