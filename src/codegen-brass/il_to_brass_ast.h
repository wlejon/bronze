#pragma once

#include "il/il.h"
#include <brass/il_translator/il_ast.hpp>
#include <string>
#include <vector>

namespace bronze::codegen {

brass::il::BronzeModuleAST lowerToBrassAst(
    const il::Module& module,
    const std::vector<std::string>& uniqueNames
);

} // namespace bronze::codegen
