// The RegExp OBJECT — the half that lives above the matcher: `lastIndex` and
// the `g`/`y` protocol around it, the shape of the match array, and the
// compiled-pattern table's identity rule.
//
// The pattern grammar and the matcher are proved in tests/regex, without a
// heap; what is pinned here is everything those cannot reach because it is
// made of Values.

#include <doctest/doctest.h>

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/builtin_regexp_internal.h"
#include "runtime/exception.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/regexp.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/symbol.h"

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

// A named member read the way a program reads it: through the property path,
// which finds 22.2.6's accessors on the real `RegExp.prototype`.
Value memberOf(Rooted<Value>& re, const char* name) {
    Rooted<Value> key{rtMakeString(name)};
    return Value(bronze_elem_get(re.get().rawBits(), key.get().rawBits()));
}

// The `lastIndex` a match would read: ToLength of the stored value.
double lastIndexOf(Rooted<Value>& re) {
    bool ok = false;
    const double n = rtRegExpLastIndexLength(re, ok);
    REQUIRE(ok);
    return n;
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
    CHECK(textOf(memberOf(re, "source")) == "a.c");
    CHECK(textOf(memberOf(re, "flags")) == "giy");
    CHECK(memberOf(re, "global").asBool());
    CHECK(memberOf(re, "ignoreCase").asBool());
    CHECK(memberOf(re, "sticky").asBool());
    CHECK_FALSE(memberOf(re, "multiline").asBool());
    CHECK_FALSE(memberOf(re, "dotAll").asBool());
    CHECK(rtRegExpText(re.get()) == "/a.c/giy");
}

TEST_CASE("source is always a pattern that could have been written as a literal") {
    ShadowStackFrame frame;
    // 22.2.6.10: only a pattern built from a STRING can contain a bare `/` or
    // a line terminator, and neither can appear between two slashes on one
    // line, so `source` carries the escaped form and the source form parses.
    Rooted<Value> slash{makeRegExp("a/b", "")};
    CHECK(textOf(memberOf(slash, "source")) == "a\\/b");
    CHECK(rtRegExpText(slash.get()) == "/a\\/b/");
    Rooted<Value> inClass{makeRegExp("[/]", "")};
    CHECK(textOf(memberOf(inClass, "source")) == "[\\/]");
    Rooted<Value> newline{makeRegExp("a\nb", "")};
    CHECK(textOf(memberOf(newline, "source")) == "a\\nb");
    // An escape already in the pattern is left alone rather than doubled.
    Rooted<Value> escaped{makeRegExp("a\\/b", "")};
    CHECK(textOf(memberOf(escaped, "source")) == "a\\/b");
    // The empty pattern, whose literal form would otherwise be a comment.
    Rooted<Value> empty{makeRegExp("", "")};
    CHECK(textOf(memberOf(empty, "source")) == "(?:)");
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
        CHECK(lastIndexOf(plain) == 0.0);
    }

    Rooted<Value> global{makeRegExp("a", "g")};
    Rooted<Value> first{rtRegExpExec(global, input)};
    CHECK(namedText(first.get(), "index") == "0");
    CHECK(lastIndexOf(global) == 1.0);
    Rooted<Value> second{rtRegExpExec(global, input)};
    CHECK(namedText(second.get(), "index") == "2");
    Rooted<Value> third{rtRegExpExec(global, input)};
    CHECK(namedText(third.get(), "index") == "4");
    // Exhausted: null, and the cursor is reset so the next call starts over.
    Rooted<Value> fourth{rtRegExpExec(global, input)};
    CHECK(fourth.get().isNull());
    CHECK(lastIndexOf(global) == 0.0);
}

TEST_CASE("a sticky pattern matches only at lastIndex") {
    ShadowStackFrame frame;
    Rooted<Value> re{makeRegExp("a", "y")};
    Rooted<Value> input{rtMakeString("Xa")};
    Rooted<Value> miss{rtRegExpExec(re, input)};
    CHECK(miss.get().isNull());
    REQUIRE(rtRegExpSetLastIndex(re, 1.0));
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
    CHECK(rtErrorText(Value(bronze_tls_block_addr()->exception_cell), text));
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

// 22.2.6's SYMBOL-keyed members are own data properties of the real
// `RegExp.prototype` (builtin_regexp_symbols.cpp). The property this pins is
// IDENTITY: a program comparing `/a/[Symbol.replace]` with
// `/b/[Symbol.replace]` must see one function object, because both reads find
// the same slot of the same prototype.

TEST_CASE("the five symbol-keyed members are one function object per key") {
    ShadowStackFrame frame;
    Rooted<Value> proto{rtRegExpPrototypeObject()};
    auto ownOf = [&](SymbolHeader* sym) {
        Rooted<Value> key{Value::fromSymbol(sym)};
        return Value(bronze_elem_get(proto.get().rawBits(), key.get().rawBits()));
    };

    Rooted<Value> match{ownOf(rtSymbolMatch())};
    Rooted<Value> matchAll{ownOf(rtSymbolMatchAll())};
    Rooted<Value> replace{ownOf(rtSymbolReplace())};
    Rooted<Value> search{ownOf(rtSymbolSearch())};
    Rooted<Value> split{ownOf(rtSymbolSplit())};

    REQUIRE(match.get().isObject());
    REQUIRE(matchAll.get().isObject());
    REQUIRE(replace.get().isObject());
    REQUIRE(search.get().isObject());
    REQUIRE(split.get().isObject());

    // Five keys, five DIFFERENT bodies. A table matched by symbol identity can
    // only be wrong by pairing a key with another's code, and these ten are
    // what that would show up as.
    CHECK(match.get().rawBits() != matchAll.get().rawBits());
    CHECK(match.get().rawBits() != replace.get().rawBits());
    CHECK(match.get().rawBits() != search.get().rawBits());
    CHECK(match.get().rawBits() != split.get().rawBits());
    CHECK(matchAll.get().rawBits() != replace.get().rawBits());
    CHECK(matchAll.get().rawBits() != search.get().rawBits());
    CHECK(matchAll.get().rawBits() != split.get().rawBits());
    CHECK(replace.get().rawBits() != search.get().rawBits());
    CHECK(replace.get().rawBits() != split.get().rawBits());
    CHECK(search.get().rawBits() != split.get().rawBits());

    // A symbol 22.2.6 does not define is not a member of the prototype.
    CHECK(ownOf(rtSymbolIterator()).isUndefined());
    CHECK(ownOf(rtSymbolSpecies()).isUndefined());
}

TEST_CASE("a symbol-keyed read of a RegExp finds 22.2.6's members") {
    ShadowStackFrame frame;

    Rooted<Value> key{Value::fromSymbol(rtSymbolReplace())};
    Rooted<Value> proto{rtRegExpPrototypeObject()};
    Rooted<Value> fromProto{Value(bronze_elem_get(proto.get().rawBits(), key.get().rawBits()))};
    Rooted<Value> re{makeRegExp("a", "g")};
    Rooted<Value> fromRead{Value(bronze_elem_get(re.get().rawBits(), key.get().rawBits()))};
    CHECK(fromRead.get().rawBits() == fromProto.get().rawBits());

    // Every RegExp's chain reaches the one prototype: a second one reads the
    // same object.
    Rooted<Value> other{makeRegExp("b", "")};
    Rooted<Value> fromOther{Value(bronze_elem_get(other.get().rawBits(), key.get().rawBits()))};
    CHECK(fromOther.get().rawBits() == fromRead.get().rawBits());

    // And only those five: a well-known key 22.2.6 does not define still reads
    // `undefined` off a RegExp, which is what 20.1.3.6 relies on.
    Rooted<Value> tag{Value::fromSymbol(rtSymbolToStringTag())};
    CHECK(Value(bronze_elem_get(re.get().rawBits(), tag.get().rawBits())).isUndefined());
}

TEST_CASE("a fresh RegExp's chain is pristine and an own key ends that") {
    ShadowStackFrame frame;
    Rooted<Value> re{makeRegExp("a", "g")};
    CHECK(rtRegExpChainPristine(re.get().asObject<RegExpHeader>()));
    // `lastIndex` lives in the header, so writing it moves no shape.
    REQUIRE(rtRegExpSetLastIndex(re, 3.0));
    CHECK(rtRegExpChainPristine(re.get().asObject<RegExpHeader>()));
    // An expando is an own key: the shape moves and the fast path is gone
    // for this instance, though not for a fresh one.
    Rooted<Value> key{rtMakeString("mine")};
    Rooted<Value> val{Value::fromDouble(1.0)};
    re.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
    CHECK_FALSE(rtRegExpChainPristine(re.get().asObject<RegExpHeader>()));
    Rooted<Value> fresh{makeRegExp("b", "")};
    CHECK(rtRegExpChainPristine(fresh.get().asObject<RegExpHeader>()));
}

TEST_CASE("RegExp.prototype.exec throws catchable RangeError on step budget exhaustion") {
    ShadowStackFrame frame;
    rtClearException();

    Rooted<Value> re{makeRegExp("(a+)+$", "")};
    Rooted<Value> input{rtMakeString("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaab")};
    Rooted<Value> result{rtRegExpExec(re, input)};

    CHECK(rtExceptionPending());
    Value exVal = Value(rtTls()->exception_cell);
    CHECK(exVal.isObject());
    std::string errText;
    CHECK(rtErrorText(exVal, errText));
    CHECK(errText.find("RangeError") != std::string::npos);
    CHECK(errText.find("backtracking") != std::string::npos);

    rtClearException();
}

TEST_CASE("RegExp compilation cache is bounded and LRU evicts cleanly") {
    ShadowStackFrame frame;

    // Create 600 distinct regular expressions
    std::vector<Rooted<Value>> regexes;
    regexes.reserve(600);
    for (int i = 0; i < 600; ++i) {
        std::string pattern = "pattern_" + std::to_string(i);
        regexes.emplace_back(makeRegExp(pattern.c_str(), ""));
    }

    // Cache must be bounded at 512
    CHECK(rtRegExpCacheSize() <= 512);

    // regexes[0] was compiled early and evicted by the subsequent 599 patterns.
    // Executing it should transparently re-compile and match accurately without crashing.
    Rooted<Value> testInput0{rtMakeString("pattern_0")};
    Rooted<Value> match0{rtRegExpExec(regexes[0], testInput0)};
    CHECK_FALSE(rtExceptionPending());
    REQUIRE(match0.get().isObject());
    CHECK(match0.get().asObject<ArrayHeader>()->length == 1);

    // Similarly test an evicted non-matching input
    Rooted<Value> testInputMiss{rtMakeString("no_match")};
    Rooted<Value> miss0{rtRegExpExec(regexes[0], testInputMiss)};
    CHECK_FALSE(rtExceptionPending());
    CHECK(miss0.get().isNull());

    // Cache size still stays <= 512
    CHECK(rtRegExpCacheSize() <= 512);
}

TEST_CASE("RegExp symbols throw catchable RangeError on ReDoS / execution limit exceeded") {
    ShadowStackFrame frame;
    rtClearException();

    Rooted<Value> re{makeRegExp("(a+)+$", "")};
    Rooted<Value> input{rtMakeString("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaab")};

    // search
    Rooted<Value> searchRes{rtRegExpSearch(re, input)};
    CHECK(rtExceptionPending());
    Value exVal = Value(rtTls()->exception_cell);
    CHECK(exVal.isObject());
    std::string errText;
    CHECK(rtErrorText(exVal, errText));
    CHECK(errText.find("RangeError") != std::string::npos);
    rtClearException();

    // replace
    Rooted<Value> replacement{rtMakeString("x")};
    Rooted<Value> replaceRes{rtRegExpReplace(re, input, replacement)};
    CHECK(rtExceptionPending());
    exVal = Value(rtTls()->exception_cell);
    CHECK(exVal.isObject());
    errText.clear();
    CHECK(rtErrorText(exVal, errText));
    CHECK(errText.find("RangeError") != std::string::npos);
    rtClearException();

    // split
    Rooted<Value> splitRes{rtRegExpSplit(re, input, Value::fromUndefined())};
    CHECK(rtExceptionPending());
    exVal = Value(rtTls()->exception_cell);
    CHECK(exVal.isObject());
    errText.clear();
    CHECK(rtErrorText(exVal, errText));
    CHECK(errText.find("RangeError") != std::string::npos);
    rtClearException();

    // match (with global flag)
    Rooted<Value> reGlobal{makeRegExp("(a+)+$", "g")};
    Rooted<Value> matchRes{rtRegExpMatch(reGlobal, input)};
    CHECK(rtExceptionPending());
    exVal = Value(rtTls()->exception_cell);
    CHECK(exVal.isObject());
    errText.clear();
    CHECK(rtErrorText(exVal, errText));
    CHECK(errText.find("RangeError") != std::string::npos);
    rtClearException();
}

