// The expression grammar — `src/parse/parser_expr.cpp`'s half of the parser:
// the precedence ladder, the two operators whose operands ECMA-262
// restricts, the optional chain, `delete` as a reference operator, and the
// one member expression the parser rewrites rather than builds a tree for.
//
// Every case is written so that a wrong grouping produces a visibly
// different dump rather than a plausible one.

// The doctest main is parser_test.cpp's; every file here links into one
// binary under the `parse` label, so the module's test command does not
// change.

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

TEST_CASE("the precedence ladder groups the new operators the way ECMA-262 does") {
    // One expression per rung, each written so a wrong grouping produces a
    // visibly different tree.
    const auto shiftBindsTighterThanRelational = parseAndDump("const a = 1 << 2 < 8;");
    CHECK(shiftBindsTighterThanRelational.find(
              "(binary <\n"
              "      (binary <<") != std::string::npos);

    // `&` above equality, `^` above `&`, `|` above `^`: `a | b ^ c & d` is
    // `a | (b ^ (c & d))`.
    const auto bitwise = parseAndDump("const a = 1 | 2 ^ 3 & 4;");
    CHECK(bitwise.find(
              "(binary |\n"
              "      (number 1)\n"
              "      (binary ^\n"
              "        (number 2)\n"
              "        (binary &\n") != std::string::npos);

    // Equality binds tighter than `&`, which is the classic surprise:
    // `1 & 2 == 2` is `1 & (2 == 2)`.
    const auto equality = parseAndDump("const a = 1 & 2 == 2;");
    CHECK(equality.find(
              "(binary &\n"
              "      (number 1)\n"
              "      (binary ==") != std::string::npos);

    // `in` and `instanceof` are relational, so additive is below them.
    const auto relational = parseAndDump("const a = 1 + 1 in o;");
    CHECK(relational.find(
              "(binary in\n"
              "      (binary +") != std::string::npos);
}

TEST_CASE("`**` is right-associative and refuses an unparenthesized unary left operand") {
    // `2 ** 3 ** 2` is 512, not 64.
    const auto right = parseAndDump("const a = 2 ** 3 ** 2;");
    CHECK(right.find(
              "(binary **\n"
              "      (number 2)\n"
              "      (binary **") != std::string::npos);

    // ECMA-262 declines to pick a reading for `-2 ** 2`, and so does bronze.
    const auto ambiguous = parseAndDump("const a = -2 ** 2;");
    CHECK(ambiguous.substr(0, 7) == "ERRORS:");
    CHECK(ambiguous.find("'**' cannot have an unparenthesized unary operand") !=
          std::string::npos);

    // Parenthesizing either way is accepted; the flag is about the source
    // form, not the node kind.
    CHECK(parseAndDump("const a = (-2) ** 2;").substr(0, 7) != "ERRORS:");
    CHECK(parseAndDump("const a = -(2 ** 2);").substr(0, 7) != "ERRORS:");
}

TEST_CASE("`??` cannot be mixed with `&&` or `||` without parentheses") {
    CHECK(parseAndDump("const a = b ?? c || d;").find(
              "'??' cannot be mixed with '&&' or '||'") != std::string::npos);
    CHECK(parseAndDump("const a = b || c ?? d;").find(
              "'??' cannot be mixed with '&&' or '||'") != std::string::npos);
    CHECK(parseAndDump("const a = b && c ?? d;").find(
              "'??' cannot be mixed with '&&' or '||'") != std::string::npos);
    CHECK(parseAndDump("const a = (b ?? c) || d;").substr(0, 7) != "ERRORS:");
}

TEST_CASE("assignment is right-associative and sits above the conditional") {
    // Both of these parsed the other way round before the precedence ladder was
    // fixed: `x = cond ? a: b` assigned the CONDITION.
    const auto ternary = parseAndDump("x = c ? 1 : 2;");
    CHECK(ternary.find(
              "(binary =\n"
              "      (ident x)\n"
              "      (ternary") != std::string::npos);

    const auto chained = parseAndDump("a = b = 3;");
    CHECK(chained.find(
              "(binary =\n"
              "      (ident a)\n"
              "      (binary =\n") != std::string::npos);
}

TEST_CASE("a comma in an argument list is a separator, not the comma operator") {
    // The hazard in adding the lowest rung: wiring comma in at the wrong
    // level turns `f(a, b)` into a one-argument call.
    const auto call = parseAndDump("f(a, b);");
    CHECK(call.find("(ident a)") != std::string::npos);
    CHECK(call.find("(ident b)") != std::string::npos);
    CHECK(call.find("(binary ,") == std::string::npos);

    const auto array = parseAndDump("const xs = [a, b];");
    CHECK(array.find("(binary ,") == std::string::npos);

    // Parenthesized, it IS the operator.
    const auto real = parseAndDump("f((a, b));");
    CHECK(real.find("(binary ,") != std::string::npos);
}

// ---- declarations, the empty statement, literals --------------

TEST_CASE("an optional chain is not an assignment or update target") {
    // ECMA-262 13.3.9: an OptionalExpression is never a valid AssignmentTarget,
    // because there is no reference to write through when the chain
    // short-circuits. Both spellings are early errors rather than a write that
    // sometimes does nothing.
    const auto assign = parseAndDump("a?.b = 1;\n");
    CHECK(assign.substr(0, 7) == "ERRORS:");
    CHECK(assign.find("an optional chain is not a valid assignment target") != std::string::npos);

    const auto inc = parseAndDump("a?.b++;\n");
    CHECK(inc.substr(0, 7) == "ERRORS:");
    CHECK(inc.find("an optional chain is not a valid target for '++' or '--'") !=
          std::string::npos);
}

TEST_CASE("`?.` before a digit is the conditional operator, not a chain") {
    // ECMA-262 12.8 gives `?.` a lookahead restriction: `a?.5:b` must lex as
    // `? .5 : b`, or the ternary with a fractional consequent stops parsing.
    const auto out = parseAndDump("const r = a ? .5 : 1;\nconst s = a?.5:1;\n");
    CHECK(out.substr(0, 7) != "ERRORS:");
    CHECK(out.find("(number 0.5)") != std::string::npos);
}

TEST_CASE("`delete` is a reference operator, and a binding is not a reference") {
    const auto prop = parseAndDump("delete o.a;\n");
    CHECK(prop.substr(0, 7) != "ERRORS:");
    CHECK(prop.find("(unary delete") != std::string::npos);

    // `delete x` is a SyntaxError in strict mode (ECMA-262 13.5.1.1), and the
    // message names the property form the author almost certainly meant.
    const auto binding = parseAndDump("const x = 1;\ndelete x;\n");
    CHECK(binding.substr(0, 7) != "ERRORS:");  // it parses; lowering refuses it
}

TEST_CASE("console members are folded by name, and an unbuilt one is loud") {
    const auto log = parseAndDump("console.log(1); console.info(2); console.debug(3);");
    CHECK(log.substr(0, 7) != "ERRORS:");
    CHECK(log.find("(ident console.info)") != std::string::npos);

    const auto warn = parseAndDump("console.warn(1); console.error(2);");
    CHECK(warn.substr(0, 7) != "ERRORS:");
    CHECK(warn.find("(ident console.warn)") != std::string::npos);
    CHECK(warn.find("(ident console.error)") != std::string::npos);

    // An unbuilt member FOLDS like a built one. The refusal moved to where
    // the unresolved-name judgment lives (lower_unresolved.cpp): lowering
    // warns by name and compiles a deferred ReferenceError, so a program
    // that carries `console.table` in a branch it never takes still runs —
    // which real bundles do (pixi's deprecation path).
    const auto count = parseAndDump("console.count();");
    CHECK(count.substr(0, 7) != "ERRORS:");
    CHECK(count.find("(ident console.count)") != std::string::npos);
}
