#pragma once

#include <doctest/doctest.h>
#include <memory>
#include <optional>
#include <string>

#include "lex/lexer.h"
#include "parse/parser.h"
#include "types/dump.h"
#include "types/infer.h"

namespace bronze::test {

struct Inferred {
    SourceBuffer buffer{"test.js", ""};
    DiagnosticSink diags;
    std::unique_ptr<ast::Module> module;
    std::optional<types::InferenceResult> result;

    std::string dump() const { return types::dump(*result); }
};

inline Inferred infer(const std::string& source) {
    Inferred out;
    out.buffer = SourceBuffer("test.js", source);
    auto tokens = Lexer(out.buffer, out.diags).lex();
    REQUIRE_FALSE(out.diags.hasErrors());
    out.module = Parser(std::move(tokens), out.diags).parseModule("test");
    REQUIRE_FALSE(out.diags.hasErrors());
    REQUIRE(out.module != nullptr);
    out.result = types::inferModule(*out.module, out.diags);
    REQUIRE_FALSE(out.diags.hasErrors());
    REQUIRE(out.result.has_value());
    return out;
}

}  // namespace bronze::test
