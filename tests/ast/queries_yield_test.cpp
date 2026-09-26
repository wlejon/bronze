#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

// The question a suspension asks of a tree: is there one under here, and of
// which forms. Named for `src/ast/queries_yield.cpp`. What is pinned is mostly
// the boundary: a nested function's `yield` is not this body's.

#include <memory>
#include <string>
#include <vector>

#include "ast/queries.h"
#include "lex/lexer.h"
#include "parse/parser.h"

using namespace bronze;

namespace {

// Parse a module and hand back the body of its FIRST generator, which is what
// the queries are asked about in the compiler.
struct Parsed {
    SourceBuffer buf{"t.ts", ""};
    DiagnosticSink diags;
    std::unique_ptr<ast::Module> mod;
};

std::shared_ptr<Parsed> parse(std::string_view src) {
    auto p = std::make_shared<Parsed>();
    p->buf = SourceBuffer("t.ts", std::string(src));
    auto tokens = Lexer(p->buf, p->diags).lex();
    REQUIRE_FALSE(p->diags.hasErrors());
    p->mod = Parser(std::move(tokens), p->diags).parseModule("t");
    REQUIRE_FALSE(p->diags.hasErrors());
    REQUIRE(p->mod != nullptr);
    return p;
}

const std::vector<ast::StmtPtr>* firstGeneratorBody(const ast::Module& mod) {
    for (const auto& s : mod.body) {
        if (const auto* fn = dynamic_cast<const ast::FunctionDecl*>(s.get())) {
            if (fn->isGenerator) return &fn->body;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("containsYield finds a suspension at any depth, and stops at a function") {
    auto flat = parse("function* g() { yield 1; }");
    const auto* body = firstGeneratorBody(*flat->mod);
    REQUIRE(body != nullptr);
    CHECK(ast::containsYield(*body));

    // Depth is irrelevant: a `yield` five blocks down is still this body's.
    auto deep = parse(
        "function* g() { if (a) { while (b) { try { for (;;) { yield 1; } } catch (e) {} } } }");
    CHECK(ast::containsYield(*firstGeneratorBody(*deep->mod)));

    // No `yield` at all.
    auto none = parse("function* g() { const x = 1; return x; }");
    CHECK_FALSE(ast::containsYield(*firstGeneratorBody(*none->mod)));

    // The boundary. `yield` inside a nested generator belongs to the nested
    // one; the outer body has no suspension point of its own.
    auto nested = parse("function* g() { function* inner() { yield 1; } return inner; }");
    CHECK_FALSE(ast::containsYield(*firstGeneratorBody(*nested->mod)));

    // Same for a function expression and an arrow written inside the body.
    auto fnExpr = parse("function* g() { const f = function () { return 1; }; return f; }");
    CHECK_FALSE(ast::containsYield(*firstGeneratorBody(*fnExpr->mod)));
    auto arrow = parse("function* g() { const f = () => 1; return f; }");
    CHECK_FALSE(ast::containsYield(*firstGeneratorBody(*arrow->mod)));

    // A class body is a boundary too: its methods are functions.
    auto cls = parse("function* g() { class K { m() { return 1; } } return K; }");
    CHECK_FALSE(ast::containsYield(*firstGeneratorBody(*cls->mod)));

    // But a `yield` that is a SIBLING of a nested function is found.
    auto sibling = parse("function* g() { const f = () => 1; yield f; }");
    CHECK(ast::containsYield(*firstGeneratorBody(*sibling->mod)));
}

TEST_CASE("yieldFormsIn separates `yield` from `yield*`") {
    // The exhaustive walk, and the reason it cannot stop at the first hit: a
    // refusal has to name every form in a position, not the first one found.
    auto plain = parse("function* g() { yield 1; }");
    CHECK(ast::yieldFormsIn(*firstGeneratorBody(*plain->mod)) == ast::YieldForms::Plain);
    CHECK_FALSE(ast::hasDelegating(ast::yieldFormsIn(*firstGeneratorBody(*plain->mod))));

    auto delegating = parse("function* g() { yield* xs; }");
    CHECK(ast::yieldFormsIn(*firstGeneratorBody(*delegating->mod)) == ast::YieldForms::Delegating);
    CHECK(ast::hasDelegating(ast::yieldFormsIn(*firstGeneratorBody(*delegating->mod))));

    // Both, and the delegation is written SECOND — an answer that quit at the
    // first suspension would report only the plain one.
    auto both = parse("function* g() { yield 1; if (a) { yield* xs; } }");
    CHECK(ast::yieldFormsIn(*firstGeneratorBody(*both->mod)) == ast::YieldForms::Both);
    CHECK(ast::hasDelegating(ast::yieldFormsIn(*firstGeneratorBody(*both->mod))));

    auto none = parse("function* g() { const x = 1; return x; }");
    CHECK(ast::yieldFormsIn(*firstGeneratorBody(*none->mod)) == ast::YieldForms::None);

    // The same boundary the boolean has: a nested generator's delegation is not
    // this body's.
    auto nested = parse("function* g() { function* i() { yield* xs; } return i; }");
    CHECK(ast::yieldFormsIn(*firstGeneratorBody(*nested->mod)) == ast::YieldForms::None);

    // A delegation nested INSIDE a suspension's operand is still this body's.
    auto inOperand = parse("function* g() { yield (yield* xs); }");
    CHECK(ast::yieldFormsIn(*firstGeneratorBody(*inOperand->mod)) == ast::YieldForms::Both);

    // The names a refusal uses. Three, because a position holding both forms
    // must not be reported as holding one of them.
    CHECK(std::string(ast::yieldFormName(ast::YieldForms::Plain)) == "a `yield`");
    CHECK(std::string(ast::yieldFormName(ast::YieldForms::Delegating)) == "a `yield*`");
    CHECK(std::string(ast::yieldFormName(ast::YieldForms::Both)) == "a `yield` or a `yield*`");
}

TEST_CASE("suspensions parse in destructuring defaults, optional chains, and compound RHS") {
    // Destructuring defaults with yield
    CHECK_NOTHROW(parse("function* g() { const [a = yield 1] = []; }"));
    CHECK_NOTHROW(parse("function* g() { const {x = yield 2} = o; }"));
    CHECK_NOTHROW(parse("function* g() { ({x = yield 3} = o); }"));
    CHECK_NOTHROW(parse("async function f() { const [a = await p] = []; }"));

    // Optional chains with yield
    CHECK_NOTHROW(parse("function* g() { return (yield 1)?.foo; }"));
    CHECK_NOTHROW(parse("function* g() { return o?.[yield 2]; }"));
    CHECK_NOTHROW(parse("function* g() { return fn?.(yield 3); }"));
    CHECK_NOTHROW(parse("function* g() { return o?.bar(yield 4); }"));
    CHECK_NOTHROW(parse("async function f() { return (await p)?.foo; }"));

    // Compound assignment with yield on RHS
    CHECK_NOTHROW(parse("function* g() { x += yield 1; }"));
    CHECK_NOTHROW(parse("function* g() { o.p += yield 2; }"));
    CHECK_NOTHROW(parse("function* g() { o[k] += yield 3; }"));
    CHECK_NOTHROW(parse("function* g() { o[yield 4] += yield 5; }"));
    CHECK_NOTHROW(parse("async function f() { x += await p; }"));
}
