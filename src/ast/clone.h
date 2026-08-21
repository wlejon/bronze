#pragma once

#include <unordered_map>

#include "ast/ast.h"

namespace bronze::ast {

// Every node of a copy, paired with the node it was copied from.
//
// Inference keys everything it proves on NODE IDENTITY (`types::InferenceResult`
// is a set of pointer-keyed maps), so a copy of a subtree is a subtree lowering
// knows nothing about: every proven type, every shape class, every merge-point
// binding and every certified field read silently reverts to the uniform
// dynamic convention. That is what a copy of a class constructor's body did to
// all 200 of three.js's constructors until this existed.
//
// It is an OPT-IN out-parameter and not a field on `Node`, because the transfer
// is only valid for a copy that will be evaluated where the original was. A
// class FIELD INITIALIZER is the counterexample and the reason the distinction
// has to be written down: inference walks it in the scope that holds the class
// declaration, and lowering copies it into the constructor, where the same free
// name can be a different value — so that copy is made with no origins and its
// facts are correctly forgotten.
using CloneOrigins = std::unordered_map<const Node*, const Node*>;

// `origins`, when given, records the whole copied subtree — every expression,
// statement and nested function body inside it, not only the root.
ExprPtr cloneExpr(const Expr& expr, CloneOrigins* origins = nullptr);
StmtPtr cloneStmt(const Stmt& stmt, CloneOrigins* origins = nullptr);
PatternPtr clonePattern(const BindingPattern& pattern, CloneOrigins* origins = nullptr);
Param cloneParam(const Param& param, CloneOrigins* origins = nullptr);
ClassMethod cloneClassMethod(const ClassMethod& method, CloneOrigins* origins = nullptr);

}  // namespace bronze::ast
