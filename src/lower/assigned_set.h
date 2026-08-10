#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "ast/ast.h"

namespace bronze::lower {

std::unordered_set<std::string> getAssignedVariables(const ast::Node& node);
std::unordered_set<std::string> getAssignedVariables(const std::vector<ast::StmtPtr>& stmts);

}  // namespace bronze::lower
