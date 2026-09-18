#pragma once

#include <string>
#include "runtime/value.h"

namespace bronze::runtime {

std::string bronze_format_stack_trace(Value errorObj, Value skipFn = Value::fromUndefined());
void bronze_install_stack(Value errorObj, Value skipFn = Value::fromUndefined());

}  // namespace bronze::runtime
