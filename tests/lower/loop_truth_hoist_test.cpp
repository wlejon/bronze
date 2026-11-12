// LOOP-INVARIANT TRUTH TESTS (src/lower/loop_truth_hoist.h): `unbox.bool` of
// a value defined outside a loop is computed once, in the loop's preheader.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "il/verifier.h"
#include "lower/guard_region_cfg.h"
#include "lower_fixture.h"

using namespace bronze;
using bronze::lower_test::parseAndLower;

namespace {

il::Module lowered(const std::string& src) {
    DiagnosticSink diags;
    SourceBuffer buf("test.js", "");
    auto mod = parseAndLower(src, diags, buf);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(mod.has_value());
    DiagnosticSink verify;
    CHECK(il::verify(*mod, verify));
    return std::move(*mod);
}

// The truth tests of function `name`: how many stand inside a loop, and how
// many outside.
struct Tests {
    size_t inLoop = 0;
    size_t outside = 0;
};
Tests truthTests(const il::Module& mod, const std::string& name) {
    Tests t;
    for (const auto& fn : mod.functions) {
        if (fn.name != name) continue;
        const lower::Cfg cfg = lower::buildCfg(fn);
        std::vector<uint8_t> inLoop(fn.blocks.size(), 0);
        for (il::BlockId header = 0; header < fn.blocks.size(); ++header) {
            for (il::BlockId pred : cfg.preds[header]) {
                if (!cfg.dominates(header, pred)) continue;
                for (il::BlockId b : lower::naturalLoop(cfg, header, pred)) inLoop[b] = 1;
            }
        }
        for (il::BlockId b = 0; b < fn.blocks.size(); ++b) {
            for (const auto& inst : fn.blocks[b].instructions) {
                if (inst.op != il::Op::Unbox || inst.type != il::Type::Bool || inst.rawUnbox || inst.nullishUnbox) continue;
                ++(inLoop[b] ? t.inLoop : t.outside);
            }
        }
    }
    return t;
}

}  // namespace

TEST_CASE("a parameter tested on every turn of a loop is tested once, ahead of it") {
    const il::Module mod = lowered(
        "function keep(list, once, context) {\n"
        "  var out = [];\n"
        "  for (var i = 0; i < list.length; i++) {\n"
        "    if (once && !list[i].once) out.push(list[i]);\n"
        "    else if (context) out.push(context);\n"
        "    if (once) out.push(1);\n"
        "  }\n"
        "  return out;\n"
        "}\n");
    const Tests t = truthTests(mod, "keep");
    // `once` (tested twice in the body, once ahead of it) and `context`; what
    // stays is the test of `list[i].once`, read on each turn.
    CHECK(t.outside == 2);
    CHECK(t.inLoop == 1);
}

TEST_CASE("a value the loop assigns keeps its test in the loop") {
    const il::Module mod = lowered(
        "function scan(list, flag) {\n"
        "  var n = 0;\n"
        "  for (var i = 0; i < list.length; i++) {\n"
        "    if (flag) n++;\n"
        "    flag = list[i];\n"
        "  }\n"
        "  return n;\n"
        "}\n");
    const Tests t = truthTests(mod, "scan");
    CHECK(t.outside == 0);
    CHECK(t.inLoop == 1);
}

TEST_CASE("a test in an inner loop of a value from outside both leaves both") {
    const il::Module mod = lowered(
        "function grid(rows, mark) {\n"
        "  var n = 0;\n"
        "  for (var r = 0; r < rows.length; r++) {\n"
        "    var row = rows[r];\n"
        "    for (var c = 0; c < row.length; c++) {\n"
        "      if (mark) n += row[c];\n"
        "      if (row) n++;\n"
        "    }\n"
        "  }\n"
        "  return n;\n"
        "}\n");
    const Tests t = truthTests(mod, "grid");
    // `mark` is tested ahead of the outer loop; `row`, read on each turn of
    // the outer loop, ahead of the inner one.
    CHECK(t.outside == 1);
    CHECK(t.inLoop == 1);
}
