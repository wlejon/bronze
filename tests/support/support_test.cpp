#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "support/diagnostics.h"
#include "support/source.h"

using namespace bronze;

TEST_CASE("SourceBuffer line/col") {
    SourceBuffer buf("t.ts", "ab\ncd\n");
    CHECK(buf.lineCol(0).line == 1);
    CHECK(buf.lineCol(0).column == 1);
    CHECK(buf.lineCol(3).line == 2);
    CHECK(buf.lineCol(4).column == 2);
}

TEST_CASE("DiagnosticSink renders and reports errors") {
    SourceBuffer buf("t.ts", "let x\n");
    DiagnosticSink sink;
    CHECK_FALSE(sink.hasErrors());
    sink.error({4, 5}, "boom");
    CHECK(sink.hasErrors());
    CHECK(sink.render(buf) == "t.ts:1:5: error: boom\n");
}
