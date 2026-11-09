#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "support/diagnostics.h"
#include "support/sha256.h"
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
 // real-looking location pointing at unrelated code.
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

TEST_CASE("sha256Hex matches the FIPS 180-4 test vectors") {
    CHECK(support::sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(support::sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // 56 bytes: the length no longer fits the first padding block.
    CHECK(support::sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(support::sha256Hex(std::string(1000, 'a')) ==
          "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}
