#pragma once

#include "ast/ast.h"

namespace bronze::ast {

ExprPtr cloneExpr(const Expr& expr);
StmtPtr cloneStmt(const Stmt& stmt);
PatternPtr clonePattern(const BindingPattern& pattern);
Param cloneParam(const Param& param);
ClassMethod cloneClassMethod(const ClassMethod& method);

}  // namespace bronze::ast
