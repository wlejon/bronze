#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>

#include "json/json.h"

using namespace bronze::json;

namespace {

ValuePtr ok(const char* text) {
    std::string error;
    Units units;
    for (const char* p = text; *p; ++p) units.push_back(static_cast<char16_t>(*p));
    ValuePtr v = parse(units, error);
    CHECK(error.empty());
    REQUIRE(v != nullptr);
    return v;
}

std::string rejected(const char* text) {
    std::string error;
    Units units;
    for (const char* p = text; *p; ++p) units.push_back(static_cast<char16_t>(*p));
    ValuePtr v = parse(units, error);
    CHECK(v == nullptr);
    CHECK(!error.empty());
    return error;
}

}  // namespace

TEST_CASE("the shapes JSON accepts") {
    CHECK(ok("null")->kind == Value::Kind::Null);
    CHECK(ok("true")->boolean == true);
    CHECK(ok("-0.5e2")->number == -50.0);
    CHECK(ok("0")->number == 0.0);
    CHECK(ok("  [ ]  ")->elements.empty());
    CHECK(ok("{}")->members.empty());

    ValuePtr nested = ok("{\"a\":[1,2,{\"b\":\"x\"}],\"c\":true}");
    REQUIRE(nested->members.size() == 2);
    CHECK(nested->members[0].key == u"a");
    CHECK(nested->members[0].value->elements.size() == 3);
    CHECK(nested->members[0].value->elements[2]->members[0].value->text == u"x");
    CHECK(nested->members[1].key == u"c");
}

TEST_CASE("escapes are per code unit") {
    CHECK(ok("\"a\\\"b\\\\c\\nd\\te\"")->text == u"a\"b\\c\nd\te");
    CHECK(ok("\"\\u0041\\u00e9\"")->text == u"A\u00e9");
    // A lone surrogate is a legal JSON string element; pairing it up or
    // replacing it would invent a value the text did not carry.
    ValuePtr lone = ok("\"\\ud800\"");
    REQUIRE(lone->text.size() == 1);
    CHECK(lone->text[0] == 0xD800);
    CHECK(ok("\"\\/\"")->text == u"/");
}

// Every one of these is legal JavaScript and none of them is JSON. This is
// the reason the module exists: sharing src/parse would have meant sharing a
// grammar that accepts them all.
TEST_CASE("the JavaScript-only spellings are rejected") {
    CHECK(rejected("{a:1}").find("double-quoted") != std::string::npos);
    CHECK(rejected("[1,2,]").find("trailing comma") != std::string::npos);
    CHECK(rejected("{\"a\":1,}").find("key") != std::string::npos);
    CHECK(!rejected("'x'").empty());
    CHECK(!rejected("0x10").empty());
    CHECK(!rejected("+1").empty());
    CHECK(!rejected(".5").empty());
    CHECK(!rejected("1.").empty());
    CHECK(!rejected("01").empty());
    CHECK(!rejected("1e").empty());
    CHECK(!rejected("// c\n1").empty());
    CHECK(!rejected("\"a\\x41\"").empty());
    CHECK(!rejected("undefined").empty());
    CHECK(!rejected("NaN").empty());
    CHECK(!rejected("Infinity").empty());
    // A raw newline inside a string is a JS template-literal thing, not a
    // JSON string character.
    CHECK(!rejected("\"a\nb\"").empty());
}

// The project rule that a parser consumes all its input or errors, applied
// here: JSON.parse('1 2') is a SyntaxError, not the number 1.
TEST_CASE("trailing content is an error, not a stopping point") {
    CHECK(!rejected("1 2").empty());
    CHECK(!rejected("{} {}").empty());
    CHECK(!rejected("").empty());
    CHECK(!rejected("   ").empty());
    CHECK(!rejected("[1,2").empty());
}

TEST_CASE("a duplicate key keeps both members, in order") {
    // The tree records what the text said; deciding that the later one wins
    // is the caller's job, and it matters because the property keeps the
    // POSITION its first definition gave it.
    ValuePtr dup = ok("{\"a\":1,\"b\":2,\"a\":3}");
    REQUIRE(dup->members.size() == 3);
    CHECK(dup->members[0].key == u"a");
    CHECK(dup->members[2].key == u"a");
    CHECK(dup->members[2].value->number == 3.0);
}
