#pragma once

#include <optional>

#include "ast/ast.h"
#include "il/il.h"
#include "support/diagnostics.h"

namespace bronze::lower {

std::optional<il::Module> lowerModule(const ast::Module& astModule, DiagnosticSink& diags);

}  // namespace bronze::lower
