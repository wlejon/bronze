#pragma once

#include <doctest/doctest.h>
#include <memory>
#include <optional>
#include <string>

#include "lex/lexer.h"
#include "parse/parser.h"
#include "types/dump.h"
#include "types/infer.h"
#include "types/pins.h"

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

// The same, under a `--pins` manifest. Pin optimism changes what a call site
// contributes — a Dynamic argument is set aside instead of joined — so a test
// that means to isolate what a NON-dynamic contribution does to a signature has
// to run the way the manifest builds do.
inline Inferred inferPinned(const std::string& source, const std::string& pinsText) {
    Inferred out;
    out.buffer = SourceBuffer("test.js", source);
    auto tokens = Lexer(out.buffer, out.diags).lex();
    REQUIRE_FALSE(out.diags.hasErrors());
    out.module = Parser(std::move(tokens), out.diags).parseModule("test");
    REQUIRE_FALSE(out.diags.hasErrors());
    REQUIRE(out.module != nullptr);
    types::PinManifest pins;
    std::string err;
    REQUIRE(pins.parse(pinsText, "test.pins", err));
    out.result = types::inferModule(*out.module, out.diags, nullptr, &pins);
    REQUIRE_FALSE(out.diags.hasErrors());
    REQUIRE(out.result.has_value());
    return out;
}

}  // namespace bronze::test
