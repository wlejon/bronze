#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

// The two questions a suspension asks of a tree: is there one under here, and
// what has to survive it. Named for `src/ast/queries_yield.cpp`.
//
// Both answers are consumed by lowering, where being wrong does not produce a
// wrong VALUE — it produces a read of an SSA value the resume edge never
// defined, which is a crash or a silently wrong program. So what is pinned here
// is mostly the boundary (a nested function's `yield` is not this body's) and
// the over-approximation (every declared name, at every depth, whether or not a
// yield could actually be reached from it).

#include <algorithm>
#include <string>
#include <vector>

#include "ast/queries.h"
#include "lex/lexer.h"
#include "parse/parser.h"

using namespace bronze;

namespace {

// Parse a module and hand back the body of its FIRST generator, which is what
// both queries are asked about in the compiler.
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

std::vector<std::string> sortedNames(const std::unordered_set<std::string>& s) {
    std::vector<std::string> out(s.begin(), s.end());
    std::sort(out.begin(), out.end());
    return out;
}

bool has(const std::unordered_set<std::string>& s, const char* name) {
    return s.count(name) != 0;
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
    // The exhaustive walk, and the reason it cannot stop at the first hit: two
    // consumers ask WHICH forms are here, and one of them — the lifter's
    // refusal — has to name every form in a position, not the first one found.
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
    // this body's, so the frame here needs no slot for one.
    auto nested = parse("function* g() { function* i() { yield* xs; } return i; }");
    CHECK(ast::yieldFormsIn(*firstGeneratorBody(*nested->mod)) == ast::YieldForms::None);

    // A delegation nested INSIDE a suspension's operand is still this body's,
    // which is what makes `yield_lift` hoist the inner one out.
    auto inOperand = parse("function* g() { yield (yield* xs); }");
    CHECK(ast::yieldFormsIn(*firstGeneratorBody(*inOperand->mod)) == ast::YieldForms::Both);

    // The names a refusal uses. Three, because a position holding both forms
    // must not be reported as holding one of them.
    CHECK(std::string(ast::yieldFormName(ast::YieldForms::Plain)) == "a `yield`");
    CHECK(std::string(ast::yieldFormName(ast::YieldForms::Delegating)) == "a `yield*`");
    CHECK(std::string(ast::yieldFormName(ast::YieldForms::Both)) == "a `yield` or a `yield*`");
}

TEST_CASE("getGeneratorFrameNames collects every declared name, at every depth") {
    auto p = parse(
        "function* g() {"
        "  let a = 1;"
        "  const b = 2;"
        "  var c = 3;"
        "  { let d = 4; }"
        "  if (x) { const e = 5; } else { let f = 6; }"
        "  for (let i = 0; i < 2; i++) { let j = i; yield j; }"
        "  while (x) { let k = 1; }"
        "  try { let m = 1; } catch (err) { let n = 2; }"
        "  function h() { let notMine = 1; }"
        "}");
    const auto names = ast::getGeneratorFrameNames(*firstGeneratorBody(*p->mod));

    for (const char* n : {"a", "b", "c", "d", "e", "f", "i", "j", "k", "m", "n"}) {
        CHECK_MESSAGE(has(names, n), (std::string("missing ") + n).c_str());
    }
    // The catch parameter is a binding of the body too: it is written by an
    // edge no join enumerates, exactly like a resume edge.
    CHECK(has(names, "err"));
    // A nested function's name is declared HERE, so it is the frame's...
    CHECK(has(names, "h"));
    // ...but the names inside it are not: they belong to its own frame.
    CHECK_FALSE(has(names, "notMine"));
}

TEST_CASE("getGeneratorFrameNames is an over-approximation, on purpose") {
    // The query is deliberately not a liveness analysis. A binding whose life
    // plainly ends before the only `yield` is still in the frame: whether it
    // crosses is a question about a control-flow graph that does not exist yet,
    // and the cost of the honest answer is a heap slot.
    auto p = parse("function* g() { let before = 1; f(before); yield 2; }");
    const auto names = ast::getGeneratorFrameNames(*firstGeneratorBody(*p->mod));
    CHECK(has(names, "before"));

    // And a generator with no yield at all still reports its bindings — the
    // caller, not the query, decides whether a frame is needed.
    auto q = parse("function* g() { let x = 1; return x; }");
    CHECK(has(ast::getGeneratorFrameNames(*firstGeneratorBody(*q->mod)), "x"));
}

TEST_CASE("getGeneratorFrameNames sees destructuring and the lifter's temporaries") {
    // A pattern declares every name in it, and the compiler needs all of them:
    // one missing binding is a read of an undefined SSA value after a resume.
    auto p = parse(
        "function* g() {"
        "  const { one, two: renamed, three = 3 } = o;"
        "  const [first, ...rest] = xs;"
        "  yield one;"
        "}");
    const auto names = ast::getGeneratorFrameNames(*firstGeneratorBody(*p->mod));
    for (const char* n : {"one", "renamed", "three", "first", "rest"}) {
        CHECK_MESSAGE(has(names, n), (std::string("missing ") + n).c_str());
    }
    // `two` is the property read, not a binding.
    CHECK_FALSE(has(names, "two"));

    // The temporaries `liftYields` introduces are ordinary `let` declarations
    // by the time this runs, so they land in the frame with everything else —
    // which is the whole point of giving them a name.
    auto q = parse("function* g() { const x = (yield 1) + (yield 2); }");
    const auto lifted = ast::getGeneratorFrameNames(*firstGeneratorBody(*q->mod));
    CHECK(has(lifted, "x"));
    const auto sorted = sortedNames(lifted);
    CHECK(std::count_if(sorted.begin(), sorted.end(), [](const std::string& n) {
              return n.rfind("gen.", 0) == 0;
          }) >= 2);
}
