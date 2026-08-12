// The `import` and `export` productions (ECMA-262 16.2), split from
// parser_test.cpp along the seam parser_module.cpp already names. Everything
// here is about which BINDINGS a module item introduces and under which
// names; what the linker then does with them is tests/modules.

// The doctest main is parser_test.cpp's; both halves link into one binary
// under the `parse` label, so the module's test command does not change.
#include <doctest/doctest.h>

#include "ast/dump.h"
#include "lex/lexer.h"
#include "parse/parser.h"

using namespace bronze;

static std::string parseAndDump(std::string_view src) {
    SourceBuffer buf("t.ts", std::string(src));
    DiagnosticSink diags;
    auto tokens = Lexer(buf, diags).lex();
    REQUIRE_FALSE(diags.hasErrors());
    auto mod = Parser(std::move(tokens), diags).parseModule("t");
    if (diags.hasErrors()) return "ERRORS:\n" + diags.render(buf);
    REQUIRE(mod != nullptr);
    return ast::dump(*mod);
}

TEST_CASE("every import clause form parses to the bindings it introduces") {
    // What `import` used to be — one hard error naming the whole construct — is
    // now four binding forms that lower differently, so each dumps under its
    // own head. The specifier is decoded like any string literal, and a
    // side-effect import binds nothing at all.
    const auto out = parseAndDump(
        "import \"./s.js\";\n"
        "import d from \"./a.js\";\n"
        "import d2, { x, y as z } from \"./b.js\";\n"
        "import * as ns from \"./c.js\";\n"
        "import { default as dd } from \"./d.js\";\n");
    CHECK(out.find("(import \"./s.js\"\n  )") != std::string::npos);
    CHECK(out.find("(default d)") != std::string::npos);
    CHECK(out.find("(named x as x)") != std::string::npos);
    CHECK(out.find("(named y as z)") != std::string::npos);
    CHECK(out.find("(namespace ns)") != std::string::npos);
    CHECK(out.find("(named default as dd)") != std::string::npos);
}

TEST_CASE("every export form records the names it publishes") {
    const auto out = parseAndDump(
        "const a = 1;\n"
        "export { a as aa };\n"
        "export let b = 2;\n"
        "export * from \"./g.js\";\n"
        "export * as gg from \"./h.js\";\n"
        "export { q } from \"./e.js\";\n"
        "export default 42;\n");
    CHECK(out.find("(name a as aa)") != std::string::npos);
    CHECK(out.find("(name b as b)") != std::string::npos);
    CHECK(out.find("(export * from \"./g.js\"") != std::string::npos);
    CHECK(out.find("(export * as gg from \"./h.js\"") != std::string::npos);
    CHECK(out.find("(export from \"./e.js\"") != std::string::npos);
    // `export default <expr>` binds a constant no source can name, so that an
    // importer has something to be renamed onto.
    CHECK(out.find("(const default") != std::string::npos);
    CHECK(out.find("(name default as default)") != std::string::npos);
}

TEST_CASE("the import forms bronze does not have are named, not mis-parsed") {
    const auto dynamicImport = parseAndDump("const m = import(\"./x.js\");");
    CHECK(dynamicImport.find("unsupported construct: dynamic import()") != std::string::npos);

    const auto meta = parseAndDump("console.log(import.meta.url);");
    CHECK(meta.find("unsupported construct: import.meta") != std::string::npos);

    // A ModuleItem is not a Statement (ECMA-262 16.2): a nested one binds
    // nothing anything outside the block could see, so there is nothing for
    // it to mean.
    const auto nested = parseAndDump("{ import { a } from \"./x.js\"; }");
    CHECK(nested.find("top level of a module") != std::string::npos);
}
