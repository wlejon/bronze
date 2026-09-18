#pragma once

#include <string>
#include "abi/bronze_abi.h"
#include "runtime/value.h"

namespace bronze::runtime {

void bronze_register_code_ranges_internal(const void* ranges, uint32_t count);
void bronze_unregister_code_ranges_internal(const void* ranges, uint32_t count);
const bronze_code_range* find_code_range(const void* pc);


std::string bronze_format_stack_trace(Value errorObj, Value skipFn = Value::fromUndefined());
void bronze_install_stack(Value errorObj, Value skipFn = Value::fromUndefined());

}  // namespace bronze::runtime
