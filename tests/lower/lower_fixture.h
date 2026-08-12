#pragma once

// The pipeline in the two shapes every lowering test needs it: with the
// inference side table and without it. Shared by the lower_*_test.cpp units
// rather than copied into each, because a copy that drifts would let two files
// disagree about what "the --no-infer path" is — which is the one distinction
// these tests exist to hold.

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "il/il.h"
#include "lex/lexer.h"
#include "lower/lower.h"
#include "parse/parser.h"
#include "support/diagnostics.h"
#include "types/infer.h"

namespace bronze::lower_test {

inline std::unique_ptr<ast::Module> parseOnly(std::string_view src, DiagnosticSink& diags,
                                              SourceBuffer& buf) {
    buf = SourceBuffer("test.ts", std::string(src));
    auto tokens = Lexer(buf, diags).lex();
    if (diags.hasErrors()) return nullptr;
    return Parser(std::move(tokens), diags).parseModule("test");
}

// Lowering with NO inference result — the `--no-infer` path. Everything is the
// uniform dynamic convention, and an annotation buys nothing, because nothing
// is proven for it to agree with.
inline std::optional<il::Module> parseAndLower(std::string_view src, DiagnosticSink& diags,
                                               SourceBuffer& buf) {
    auto astMod = parseOnly(src, diags, buf);
    if (diags.hasErrors() || !astMod) return std::nullopt;
    return lower::lowerModule(*astMod, diags);
}

// The real pipeline: inference runs first and lowering consumes the side table.
inline std::optional<il::Module> inferAndLower(std::string_view src, DiagnosticSink& diags,
                                               SourceBuffer& buf) {
    auto astMod = parseOnly(src, diags, buf);
    if (diags.hasErrors() || !astMod) return std::nullopt;
    auto inferred = types::inferModule(*astMod, diags);
    if (diags.hasErrors() || !inferred) return std::nullopt;
    return lower::lowerModule(*astMod, diags, &*inferred);
}

}  // namespace bronze::lower_test
