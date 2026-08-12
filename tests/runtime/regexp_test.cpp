// The RegExp OBJECT — the half that lives above the matcher: `lastIndex` and
// the `g`/`y` protocol around it, the shape of the match array, and the
// compiled-pattern table's identity rule.
//
// The pattern grammar and the matcher are proved in tests/regex, without a
// heap; what is pinned here is everything those cannot reach because it is
// made of Values.

#include <doctest/doctest.h>

#include <string>

#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/regexp.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"

using namespace bronze;
using namespace bronze::runtime;

// `Rooted` registers into the CURRENT ShadowStackFrame and does nothing when
// there is none, so every case below opens one first. Without it the values
// here would be unrooted, and the gc-stress run — which collects at every
// allocation — would read them after they had been collected.

namespace {

Value makeRegExp(const char* source, const char* flags) {
    Rooted<Value> src{rtMakeString(source)};
    return rtRegExpFromParts(src, flags);
}

std::string textOf(Value v) {
    if (!v.isString()) return "<not a string>";
    return rtUtf8Chars(v.asString<StringHeader>());
}

// A match array's element `i`, as text, or a marker for the two ways it can
// not be one.
std::string elementText(Value array, uint32_t i) {
    if (!array.isObject()) return "<null>";
    Value v = array.asObject<ArrayHeader>()->getElem(i);
    if (v.isUndefined()) return "<undefined>";
    return textOf(v);
}

std::string namedText(Value array, const char* name) {
    Rooted<Value> props{array.asObject<ArrayHeader>()->properties};
    if (!props.get().isObject()) return "<no properties>";
    Rooted<Value> key{rtMakeString(name)};
    Value v = props.get().asObject<ObjectHeader>()->getProp(rtHeap(), key);
    if (v.isUndefined()) return "<undefined>";
    if (v.isNumber()) return std::to_string(static_cast<int64_t>(v.asNumber()));
    return textOf(v);
}

}  // namespace

TEST_CASE("source and flags round-trip in 22.2.6.5's order") {
    ShadowStackFrame frame;
    Rooted<Value> re{makeRegExp("a.c", "yig")};
    CHECK(textOf(rtRegExpMember(re.get(), "source")) == "a.c");
    CHECK(textOf(rtRegExpMember(re.get(), "flags")) == "giy");
    CHECK(rtRegExpMember(re.get(), "global").asBool());
    CHECK(rtRegExpMember(re.get(), "ignoreCase").asBool());
    CHECK(rtRegExpMember(re.get(), "sticky").asBool());
    CHECK_FALSE(rtRegExpMember(re.get(), "multiline").asBool());
    CHECK_FALSE(rtRegExpMember(re.get(), "dotAll").asBool());
    CHECK(rtRegExpText(re.get()) == "/a.c/giy");
}

TEST_CASE("source is always a pattern that could have been written as a literal") {
    ShadowStackFrame frame;
    // 22.2.6.10: only a pattern built from a STRING can contain a bare `/` or
    // a line terminator, and neither can appear between two slashes on one
    // line, so `source` carries the escaped form and the source form parses.
    Rooted<Value> slash{makeRegExp("a/b", "")};
    CHECK(textOf(rtRegExpMember(slash.get(), "source")) == "a\\/b");
    CHECK(rtRegExpText(slash.get()) == "/a\\/b/");
    Rooted<Value> inClass{makeRegExp("[/]", "")};
    CHECK(textOf(rtRegExpMember(inClass.get(), "source")) == "[\\/]");
    Rooted<Value> newline{makeRegExp("a\nb", "")};
    CHECK(textOf(rtRegExpMember(newline.get(), "source")) == "a\\nb");
    // An escape already in the pattern is left alone rather than doubled.
    Rooted<Value> escaped{makeRegExp("a\\/b", "")};
    CHECK(textOf(rtRegExpMember(escaped.get(), "source")) == "a\\/b");
    // The empty pattern, whose literal form would otherwise be a comment.
    Rooted<Value> empty{makeRegExp("", "")};
    CHECK(textOf(rtRegExpMember(empty.get(), "source")) == "(?:)");
}

TEST_CASE("two regular expressions with the same source share one compilation") {
    ShadowStackFrame frame;
    Rooted<Value> a{makeRegExp("x(y)z", "g")};
    Rooted<Value> b{makeRegExp("x(y)z", "g")};
    CHECK(&rtRegExpPattern(a.get()) == &rtRegExpPattern(b.get()));
    // The flags are part of the identity: `i` compiles different Char nodes.
    Rooted<Value> c{makeRegExp("x(y)z", "gi")};
    CHECK(&rtRegExpPattern(a.get()) != &rtRegExpPattern(c.get()));
    // ...but the ORDER the flags were written in is not.
    Rooted<Value> d{makeRegExp("x(y)z", "ig")};
    CHECK(&rtRegExpPattern(c.get()) == &rtRegExpPattern(d.get()));
}

TEST_CASE("the match array carries index, input and groups") {
    ShadowStackFrame frame;
    Rooted<Value> re{makeRegExp("(?<word>[a-z]+)(\\d)?", "")};
    Rooted<Value> input{rtMakeString("  abc")};
    Rooted<Value> match{rtRegExpExec(re, input)};
    REQUIRE(match.get().isObject());
    CHECK(elementText(match.get(), 0) == "abc");
    CHECK(elementText(match.get(), 1) == "abc");
    // A group that did not participate is `undefined`, not "".
    CHECK(elementText(match.get(), 2) == "<undefined>");
    CHECK(namedText(match.get(), "index") == "2");
    CHECK(namedText(match.get(), "input") == "  abc");
    Rooted<Value> props{match.get().asObject<ArrayHeader>()->properties};
    Rooted<Value> groupsKey{rtMakeString("groups")};
    Value groups = props.get().asObject<ObjectHeader>()->getProp(rtHeap(), groupsKey);
    REQUIRE(groups.isObject());
    Rooted<Value> groupsRoot{groups};
    Rooted<Value> nameKey{rtMakeString("word")};
    CHECK(textOf(groupsRoot.get().asObject<ObjectHeader>()->getProp(rtHeap(), nameKey)) == "abc");
}

TEST_CASE("groups is undefined when the pattern has no named group") {
    ShadowStackFrame frame;
    Rooted<Value> re{makeRegExp("a", "")};
    Rooted<Value> input{rtMakeString("a")};
    Rooted<Value> match{rtRegExpExec(re, input)};
    REQUIRE(match.get().isObject());
    CHECK(namedText(match.get(), "groups") == "<undefined>");
}

TEST_CASE("lastIndex is a cursor for g and y and is ignored otherwise") {
    ShadowStackFrame frame;
    Rooted<Value> input{rtMakeString("aXaXa")};

    Rooted<Value> plain{makeRegExp("a", "")};
    for (int i = 0; i < 3; ++i) {
        Rooted<Value> match{rtRegExpExec(plain, input)};
        CHECK(namedText(match.get(), "index") == "0");
        CHECK(rtRegExpLastIndex(plain.get()) == 0.0);
    }

    Rooted<Value> global{makeRegExp("a", "g")};
    Rooted<Value> first{rtRegExpExec(global, input)};
    CHECK(namedText(first.get(), "index") == "0");
    CHECK(rtRegExpLastIndex(global.get()) == 1.0);
    Rooted<Value> second{rtRegExpExec(global, input)};
    CHECK(namedText(second.get(), "index") == "2");
    Rooted<Value> third{rtRegExpExec(global, input)};
    CHECK(namedText(third.get(), "index") == "4");
    // Exhausted: null, and the cursor is reset so the next call starts over.
    Rooted<Value> fourth{rtRegExpExec(global, input)};
    CHECK(fourth.get().isNull());
    CHECK(rtRegExpLastIndex(global.get()) == 0.0);
}

TEST_CASE("a sticky pattern matches only at lastIndex") {
    ShadowStackFrame frame;
    Rooted<Value> re{makeRegExp("a", "y")};
    Rooted<Value> input{rtMakeString("Xa")};
    Rooted<Value> miss{rtRegExpExec(re, input)};
    CHECK(miss.get().isNull());
    rtRegExpSetLastIndex(re.get(), 1.0);
    Rooted<Value> hit{rtRegExpExec(re, input)};
    REQUIRE(hit.get().isObject());
    CHECK(namedText(hit.get(), "index") == "1");
}

TEST_CASE("a pattern that does not compile is a catchable SyntaxError") {
    ShadowStackFrame frame;
    Rooted<Value> bad{makeRegExp("(unclosed", "")};
    CHECK(bad.get().isUndefined());
    REQUIRE(rtExceptionPending());
    std::string text;
    CHECK(rtErrorText(Value(bronze_exception_cell), text));
    CHECK(text.find("SyntaxError") == 0);
    CHECK(text.find("Invalid regular expression") != std::string::npos);
    rtClearException();
}

TEST_CASE("console.log prints a regular expression as its source form") {
    ShadowStackFrame frame;
    Rooted<Value> re{makeRegExp("ab+", "gi")};
    CHECK(rtInspect(re.get()) == "/ab+/gi");
    // A match array prints its named properties after its elements.
    Rooted<Value> plain{makeRegExp("b", "")};
    Rooted<Value> input{rtMakeString("ab")};
    Rooted<Value> match{rtRegExpExec(plain, input)};
    CHECK(rtInspect(match.get()) == "[ 'b', index: 1, input: 'ab', groups: undefined ]");
}
