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

TEST_CASE("a span names the buffer it indexes, so a graph renders per file") {
    // The same byte offset in two files is two different line/column pairs
    // and two different names. Rendering both against one buffer produced a
    // real-looking location pointing at unrelated code (docs/0023 dec. 1).
    SourceSet sources;
    sources.add("entry.js", "let x = 1;\n");
    sources.add("lib.js", "\n\nlet y = 2;\n");
    CHECK(sources.size() == 2);

    DiagnosticSink sink;
    sink.error({4, 5, 0}, "in the entry");
    sink.error({4, 5, 1}, "in the dependency");
    CHECK(sink.render(sources) ==
          "entry.js:1:5: error: in the entry\n"
          "lib.js:3:3: error: in the dependency\n");

    // A default-constructed Span is file 0, which is why every span written
    // before bronze had a graph still renders against the file the user named.
    DiagnosticSink plain;
    plain.error(Span{}, "no location");
    CHECK(plain.render(sources) == "entry.js:1:1: error: no location\n");
}
