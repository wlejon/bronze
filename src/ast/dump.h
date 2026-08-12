#pragma once
#include <string>

#include "ast/ast.h"

namespace bronze::ast {

// Canonical, deterministic s-expression dump of an AST. This is the seed of
// the differential-testing discipline: every stage has a canonical text form
// that tests compare byte-for-byte.
std::string dump(const Module& module);

}  // namespace bronze::ast
