// ECMA-262 14.7.4.9 CreatePerIterationEnvironment, as `src/lower/lower_control.cpp`
// builds it: the `for` head's environment record is not threaded round the back
// edge, it is COPIED into a sibling before every iteration, and the loop carries
// the copy as one more block parameter than it has variables.
//
// The facts worth pinning separately from the oracle case are the ones the
// output cannot show: that the copies are absent from every loop no closure
// reaches (which is what keeps an ordinary counted loop allocation-free), that
// the head's own record is never any iteration's, and that the copy is emitted
// above the increment rather than below it.

#include <doctest/doctest.h>

#include <string>

#include "il/print.h"
#include "lower_fixture.h"

using namespace bronze;
using bronze::lower_test::inferAndLower;
using bronze::lower_test::parseAndLower;

namespace {

size_t countOf(const std::string& haystack, const std::string& needle) {
    size_t n = 0;
    for (size_t at = haystack.find(needle); at != std::string::npos;
         at = haystack.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
}

const il::Function* functionNamed(const il::Module& mod, const std::string& name) {
    for (const auto& fn : mod.functions) {
        if (fn.name == name) return &fn;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("a counted for loop no closure reaches builds no environment at all") {
    // The copy is observable through a closure and through nothing else, so a
    // loop with none must keep the shape it had: the binding lives in SSA and
    // the back edge's block argument already IS one value per iteration.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "let sum = 0;\n"
        "for (let i = 0; i < 3; i++) { sum += i; }\n"
        "console.log(sum);\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    REQUIRE_FALSE(diags.hasErrors());
    CHECK(il::print(*optMod).find("env.create") == std::string::npos);
}

TEST_CASE("a for binding a closure reaches is copied before every iteration") {
    // Three records for a loop with one head binding: the one the head's
    // declarations ran in, the entry copy (ForBodyEvaluation step 2, before the
    // first test), and the one the update block makes on each trip round.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "const a = [];\n"
        "for (let i = 0; i < 3; i++) { a.push(() => i); }\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    REQUIRE_FALSE(diags.hasErrors());
    const std::string text = il::print(*optMod);
    CHECK(countOf(text, "env.create") == 3);
    // Each copy reads the slot it is copying and writes it into the new record.
    CHECK(countOf(text, "env.get %") == 2);
}

TEST_CASE("the per-iteration record is the header's block parameter, not a variable") {
    // The environment joins at the header like a loop variable, but it is not
    // one: it is appended AFTER them, so that `collectEdgeArgs`'s positional
    // match between the variable list and the target's parameters still holds.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower(
        "const a = [];\n"
        "let n = 0;\n"
        "for (let i = 0; i < 3; i++) { n = n + 1; a.push(() => i); }\n"
        "console.log(n);\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    REQUIRE_FALSE(diags.hasErrors());
    const il::Function* main = functionNamed(*optMod, "main");
    REQUIRE(main != nullptr);

    // `n` is the one loop variable, so the header takes two parameters and the
    // last of them is the record — a dynamic, whatever `n` was proven to be.
    const il::Block* header = nullptr;
    for (const auto& block : main->blocks) {
        if (block.params.size() == 2) header = &block;
    }
    REQUIRE(header != nullptr);
    CHECK(header->params.back().type == il::Type::Dynamic);
    // The exit block is NOT among them: the loop's scope is over by then, so it
    // carries the variables and nothing else.
    for (const auto& block : main->blocks) {
        CHECK(block.params.size() <= 2);
    }
}

TEST_CASE("a closure in the head captures the head's own record, never a copy") {
    // The subtle half of 14.7.4.9: the copies are made AFTER the head's
    // declarations run, and the increment writes only to copies — so the record
    // the head ran in holds the initial value forever. Which is visible here as
    // the ordering: the closure is created before the first `env.create` that
    // copies anything.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "let first;\n"
        "for (let i = 0, f = () => i; i < 3; i++) { if (i === 0) first = f; }\n"
        "console.log(first());\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    REQUIRE_FALSE(diags.hasErrors());
    const std::string text = il::print(*optMod);
    const size_t closure = text.find("create.func");
    const size_t firstCopy = text.find("env.get %");
    REQUIRE(closure != std::string::npos);
    REQUIRE(firstCopy != std::string::npos);
    CHECK(closure < firstCopy);
}

TEST_CASE("the update block copies before it increments") {
    // Step 3.e before step 3.f. Getting these the other way round is not a
    // reordering: it is the difference between the arrow of iteration 0 seeing
    // 0 and seeing 1.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "const a = [];\n"
        "for (let i = 0; i < 3; i++) { a.push(() => i); }\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    REQUIRE_FALSE(diags.hasErrors());
    const il::Function* main = functionNamed(*optMod, "main");
    REQUIRE(main != nullptr);

    // The update block is the one holding both a copy and the increment. A
    // captured `i` reads back dynamic, so `i++` is the ToNumeric step rather
    // than a typed add — either opcode counts as the increment here; the
    // ordering is what this case pins.
    bool sawUpdate = false;
    for (const auto& block : main->blocks) {
        size_t create = block.instructions.size();
        size_t step = block.instructions.size();
        for (size_t i = 0; i < block.instructions.size(); ++i) {
            const il::Op op = block.instructions[i].op;
            if (op == il::Op::EnvCreate && create == block.instructions.size()) create = i;
            if ((op == il::Op::Add || op == il::Op::NumericStep) && step == block.instructions.size())
                step = i;
        }
        if (create == block.instructions.size() || step == block.instructions.size()) continue;
        sawUpdate = true;
        CHECK(create < step);
    }
    CHECK(sawUpdate);
}

TEST_CASE("a binding in a record only because a handler may read it is not copied") {
    // `memoryNames_` puts a name assigned inside a `try` in an environment slot
    // so that the handler can reach it, which is a different question from
    // 14.7.4.9's. Copying for it would be allocation per iteration that no
    // program can observe.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function f(n) {\n"
        "  let s = 0;\n"
        "  try { for (let i = 0; i < n; i++) { s += i; } } catch (e) { s = -1; }\n"
        "  return s;\n"
        "}\n"
        "console.log(f(3));\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    REQUIRE_FALSE(diags.hasErrors());
    // Whatever records this function makes, none of them is a copy of another:
    // a copy is the only thing that reads a slot to write the same slot.
    CHECK(il::print(*optMod).find("env.get %") == std::string::npos);
}

TEST_CASE("a closure that binds the loop's name itself gets no copies") {
    // The three.js shape. A callback whose parameter is called `i` shares a
    // spelling with the loop's binding and nothing else, so the loop must not
    // pay for a per-iteration record — and the head's record still exists,
    // because the enclosing scope's `i` is what it shadows.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "const items = [10, 20];\n"
        "let total = 0;\n"
        "for (let i = 0, il = items.length; i < il; i++) { total += items[i]; }\n"
        "const labels = items.map(function (x, i) { return i + \":\" + x; });\n"
        "console.log(total, labels.join(\",\"));\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    REQUIRE_FALSE(diags.hasErrors());
    CHECK(il::print(*optMod).find("env.get %") == std::string::npos);
}

TEST_CASE("a continue hands the per-iteration record to the update block") {
    // A `continue` is an edge into the update block like the body's
    // fall-through, so it carries the same list — including the record, which
    // is the header's and not whatever is innermost where the `continue` is
    // written.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "const a = [];\n"
        "for (let i = 0; i < 4; i++) { if (i === 2) continue; a.push(() => i); }\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    REQUIRE_FALSE(diags.hasErrors());
    const il::Function* main = functionNamed(*optMod, "main");
    REQUIRE(main != nullptr);

    // Every edge into a block agrees with its parameter list — the verifier's
    // rule, checked here on the block the `continue` and the fall-through both
    // reach, because a missing environment argument is exactly what an extra
    // parameter invites.
    size_t edgesChecked = 0;
    for (const auto& block : main->blocks) {
        for (const auto& inst : block.instructions) {
            for (const il::BlockTarget* target : {&inst.target, &inst.elseTarget}) {
                if (target->block == il::kNoBlock) continue;
                CHECK(target->args.size() == main->blocks[target->block].params.size());
                ++edgesChecked;
            }
        }
    }
    CHECK(edgesChecked > 0);
    // Two edges into the update block, both carrying the record.
    CHECK(countOf(il::print(*optMod), "env.create") == 3);
}
