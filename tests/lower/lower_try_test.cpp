// The IL shape of `try` / `catch` / `finally` / `throw`.
//
// The oracle cases pin what a program PRINTS. These pin the structure that
// makes those answers true, because two of the three bugs found while
// building this were invisible in the output and visible in the dump: a
// `finally` copy running under its own handler, and a `main` that grew a
// handler block it had no use for.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "il/print.h"
#include "lex/lexer.h"
#include "lower/lower.h"
#include "parse/parser.h"

using namespace bronze;

namespace {

// The `--no-infer` path throughout: the handler edges are a fact about
// control flow, and nothing inference proves may move one.
std::string printOf(std::string_view src) {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", std::string(src));
    auto tokens = Lexer(buf, diags).lex();
    REQUIRE_FALSE(diags.hasErrors());
    auto astMod = Parser(std::move(tokens), diags).parseModule("test");
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(astMod != nullptr);
    const auto optMod = lower::lowerModule(*astMod, diags);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    return il::print(*optMod);
}

size_t countOf(const std::string& haystack, const std::string& needle) {
    size_t n = 0;
    for (size_t at = haystack.find(needle); at != std::string::npos;
         at = haystack.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
}

// One block of the canonical text form, split into the three things these
// tests ask about. Parsed rather than string-matched because the questions
// are structural — which handler this block names, where it goes next — and a
// substring search over the whole dump cannot tell two blocks apart.
struct Block {
    std::string name;
    std::string handler;  // empty when the block names none
    std::string body;
};

std::vector<Block> blocksOf(const std::string& printed, const std::string& func) {
    std::vector<Block> blocks;
    const size_t funcAt = printed.find("\nfunc " + func + "(");
    if (funcAt == std::string::npos) return blocks;
    const size_t funcEnd = printed.find("\n}\n", funcAt);
    const std::string text = printed.substr(funcAt, funcEnd - funcAt);

    size_t at = 0;
    while ((at = text.find("\n  b", at)) != std::string::npos) {
        const size_t headEnd = text.find(":\n", at);
        if (headEnd == std::string::npos) break;
        const std::string head = text.substr(at + 3, headEnd - at - 3);
        Block b;
        const size_t space = head.find(' ');
        b.name = space == std::string::npos ? head : head.substr(0, space);
        if (space != std::string::npos) b.handler = head.substr(head.rfind(' ') + 1);
        size_t bodyEnd = text.find("\n  b", headEnd);
        if (bodyEnd == std::string::npos) bodyEnd = text.size();
        b.body = text.substr(headEnd + 2, bodyEnd - headEnd - 2);
        blocks.push_back(std::move(b));
        at = headEnd;
    }
    return blocks;
}

const Block* blockNamed(const std::vector<Block>& blocks, const std::string& name) {
    for (const Block& b : blocks) {
        if (b.name == name) return &b;
    }
    return nullptr;
}

bool bodyHas(const Block& b, const std::string& needle) {
    return b.body.find(needle) != std::string::npos;
}

// The target of a block that ends in an unconditional jump.
std::string jumpTargetOf(const Block& b) {
    const size_t at = b.body.rfind("jump b");
    if (at == std::string::npos) return "";
    const size_t start = at + 5;
    size_t end = start;
    while (end < b.body.size() && b.body[end] != '\n' && b.body[end] != '(') ++end;
    return b.body.substr(start, end - start);
}

}  // namespace

TEST_CASE("a protected region's blocks name the catch as their handler") {
    // The handler is a property of the BLOCK, not an operand of each
    // instruction, so `try` needs a block of its own — carrying on in the block
    // the statement was reached in would put the enclosing handler's edge on
    // the first thing the body does.
    const std::string printed = printOf("function f(g) { try { g(); } catch (e) { g(e); } }");

    // Exactly one block is annotated, and it is the protected one: the
    // handler block and the join must NOT be inside their own try.
    CHECK(countOf(printed, " handler b") == 1);
    // The handler starts by taking the pending value, and that is the only
    // exc.take: the cell is read once per handler.
    CHECK(countOf(printed, "exc.take") == 1);

    const std::vector<Block> blocks = blocksOf(printed, "f");
    const Block* protectedBlock = nullptr;
    for (const Block& b : blocks) {
        if (!b.handler.empty()) protectedBlock = &b;
    }
    REQUIRE(protectedBlock != nullptr);
    const Block* handler = blockNamed(blocks, protectedBlock->handler);
    REQUIRE(handler != nullptr);
    CHECK(bodyHas(*handler, "exc.take"));
    // A handler block takes no parameters: it is entered from an arbitrary
    // point in the protected region, so there is no edge to pass them on at an
    // arbitrary point in the protected region.
    CHECK(handler->name.find('(') == std::string::npos);
}

TEST_CASE("a throw is a terminator that names no target") {
    const std::string printed = printOf("function f(x) { throw x; }");
    CHECK(printed.find("throw %") != std::string::npos);
    // The edge is on the block, not on the instruction, so a `throw` never
    // prints a block target the way `jump` and `br` do.
    CHECK(printed.find("throw %0, b") == std::string::npos);
}

TEST_CASE("main gets no handler block when nothing in it can throw") {
    // The entry point's unwind path is a property of the FUNCTION (it reports
    // and exits rather than returning), so it costs no IL. An `uncaught.throw`
    // block in every `main` would have changed every pinned dump in the suite
    // for a program that never throws.
    const std::string printed = printOf("const a = 10 + 20;");
    CHECK(printed.find("handler") == std::string::npos);
    CHECK(printed.find("func main() -> void {\n  b0:\n") != std::string::npos);
    CHECK(countOf(printed, "\n  b") == 1);
}

TEST_CASE("a finally is duplicated per exit path, under the OUTER handler") {
    // No completion record and no dispatch, so each way out of the protected
    // region gets its own copy. Two exits here — normal completion and the
    // exception path — so the body appears twice.
    const std::string printed = printOf("function f(g) { try { g(1); } finally { g(2); } }");
    CHECK(countOf(printed, "const.f64 2") == 2);

    // And the copies run OUTSIDE the try: a finally that throws must
    // propagate outward, not re-enter the handler that is running it. The bug
    // this pins made the normal-path copy name the try's own handler, which
    // would have run the finally a second time on its own exception.
    CHECK(countOf(printed, " handler b") == 1);
    const std::vector<Block> blocks = blocksOf(printed, "f");
    for (const Block& b : blocks) {
        if (bodyHas(b, "const.f64 2")) CHECK(b.handler.empty());
    }
}

TEST_CASE("a return crossing two finallys runs both, innermost first") {
    // `g(1)` is the inner finally and `g(2)` the outer one. The try body
    // always returns, so the inner statement has no reachable normal
    // completion and gets two copies (the return path and the exception
    // path); the outer statement gets three, because all three of the inner
    // one's exits leave through it.
    const std::string printed = printOf(
        "function f(g) {"
        "  try { try { return g(0); } finally { g(1); } } finally { g(2); }"
        "}");
    CHECK(countOf(printed, "const.f64 1") == 2);
    CHECK(countOf(printed, "const.f64 2") == 3);

    const std::vector<Block> blocks = blocksOf(printed, "f");
    REQUIRE(!blocks.empty());

    // The return path, followed edge by edge: the inner finally's copy runs
    // first and hands control to the outer one, which is where the `ret`
    // happens. 14.15.3 nests the two statements, and this edge is the whole
    // of what "innermost first" means.
    const Block* innerCopy = nullptr;
    for (const Block& b : blocks) {
        if (bodyHas(b, "const.f64 1") && !bodyHas(b, "exc.take")) innerCopy = &b;
    }
    REQUIRE(innerCopy != nullptr);
    const Block* outerCopy = blockNamed(blocks, jumpTargetOf(*innerCopy));
    REQUIRE(outerCopy != nullptr);
    CHECK(bodyHas(*outerCopy, "const.f64 2"));
    CHECK(bodyHas(*outerCopy, "ret %"));

    // Neither copy runs under the handler of the statement it belongs to: the
    // inner one is protected by the OUTER try, and the outer one by nothing.
    CHECK_FALSE(innerCopy->handler.empty());
    CHECK(outerCopy->handler.empty());
    const Block* outerHandler = blockNamed(blocks, innerCopy->handler);
    REQUIRE(outerHandler != nullptr);
    CHECK(bodyHas(*outerHandler, "exc.take"));
    CHECK(bodyHas(*outerHandler, "const.f64 2"));
}

TEST_CASE("a break crossing a finally runs it before leaving the loop") {
    const std::string printed = printOf(
        "function f(g) {"
        "  for (let i = 0; i < 3; i = i + 1) { try { break; } finally { g(9); } }"
        "}");
    // Three copies: the one in front of the `break`, the unreachable
    // fall-through path, and the exception path.
    CHECK(countOf(printed, "const.f64 9") == 3);
}

TEST_CASE("a binding a try assigns lives in an environment record") {
    // The handler block is entered from an arbitrary point in the protected
    // region, so no block-argument join can carry a value the body
    // half-updated. Assignments a `try` makes go through the environment
    // instead, which is what makes both the handler and the join parameterless.
    const std::string printed = printOf(
        "function f(g) { let x = 1; try { x = g(); } catch (e) { x = 2; } return x; }");
    CHECK(printed.find("env.set") != std::string::npos);
    CHECK(printed.find("env.get") != std::string::npos);
    // No block anywhere in the function takes parameters.
    for (const Block& b : blocksOf(printed, "f")) {
        CHECK(b.name.find('(') == std::string::npos);
    }

    // A binding a try does NOT assign stays a plain SSA value: making every
    // local a cell would undo the closure analysis's narrowing. `y` below is
    // read after the statement and never written inside it, so it reaches the
    // join as an SSA value rather than through memory.
    const std::string untouched =
        printOf("function f(g) { let y = 1; try { g(); } catch (e) { g(e); } return y; }");
    const std::vector<Block> blocks = blocksOf(untouched, "f");
    REQUIRE(!blocks.empty());
    for (const Block& b : blocks) {
        // The one environment record here belongs to the CATCH PARAMETER,
        // which gets the declarative environment 14.15.2 gives it — so every
        // env.set is in the handler, and none is on the path `y` travels.
        if (bodyHas(b, "env.set")) CHECK(bodyHas(b, "exc.take"));
    }
}
