#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/rt_builtins.h"
#include "runtime/string.h"
#include "runtime/value.h"

using namespace bronze;

TEST_CASE("string creation latin1 vs utf16") {
    Heap heap;

    StringHeader* s1 = StringHeader::createLatin1(heap, "Hello", 5);
    REQUIRE(s1 != nullptr);
    CHECK(s1->length == 5);
    CHECK(s1->getLength() == 5);
    CHECK(s1->isLatin1());
    CHECK(!s1->isUTF16());
    CHECK(s1->charCodeAt(0) == 'H');
    CHECK(s1->charCodeAt(4) == 'o');

    uint16_t u16_buf[] = {'W', 'o', 'r', 'l', 'd'};
    StringHeader* s2 = StringHeader::createUTF16(heap, u16_buf, 5);
    REQUIRE(s2 != nullptr);
    CHECK(s2->length == 5);
    CHECK(s2->isUTF16());
    CHECK(!s2->isLatin1());
    CHECK(s2->charCodeAt(0) == 'W');
    CHECK(s2->charCodeAt(4) == 'd');

    StringHeader* s3 = StringHeader::createFromUTF8(heap, "Hello");
    CHECK(s3->isLatin1());
    CHECK(s3->length == 5);

    StringHeader* s4 = StringHeader::createFromUTF8(heap, "Café");
    CHECK(s4->isLatin1());
    CHECK(s4->length == 4);
    CHECK(s4->charCodeAt(3) == 233);

    StringHeader* s5 = StringHeader::createFromUTF8(heap, "Hello 世界");
    CHECK(s5->isUTF16());
    CHECK(s5->length == 8);

    StringHeader* s6 = StringHeader::createFromUTF8(heap, "Hi 🌍");
    CHECK(s6->isUTF16());
    CHECK(s6->length == 5);
}

TEST_CASE("non-ascii string indexing and surrogate pair access") {
    Heap heap;

    StringHeader* s1 = StringHeader::createFromUTF8(heap, "Café");
    CHECK(s1->length == 4);
    CHECK(s1->charCodeAt(0) == 'C');
    CHECK(s1->charCodeAt(1) == 'a');
    CHECK(s1->charCodeAt(2) == 'f');
    CHECK(s1->charCodeAt(3) == 233);
    CHECK(s1->charAt(3) == 233);
    CHECK(s1->charCodeAt(4) == 0);

    StringHeader* emoji = StringHeader::createFromUTF8(heap, "🌍");
    CHECK(emoji->length == 2);
    CHECK(emoji->charCodeAt(0) == 0xD83C);
    CHECK(emoji->charCodeAt(1) == 0xDF0D);
    CHECK(emoji->charCodeAt(2) == 0);
}

TEST_CASE("string equality and mixed representation equality") {
    Heap heap;
    ShadowStackFrame frame;

    Rooted<StringHeader*> lat1(StringHeader::createFromUTF8(heap, "TestString"));
    Rooted<StringHeader*> lat2(StringHeader::createFromUTF8(heap, "TestString"));
    Rooted<StringHeader*> lat3(StringHeader::createFromUTF8(heap, "OtherString"));

    CHECK(lat1.get()->equals(*lat2.get()));
    CHECK(lat2.get()->equals(*lat1.get()));
    CHECK(!lat1.get()->equals(*lat3.get()));

    uint16_t u16_buf[] = {'T', 'e', 's', 't', 'S', 't', 'r', 'i', 'n', 'g'};
    Rooted<StringHeader*> utf1(StringHeader::createUTF16(heap, u16_buf, 10));

    CHECK(utf1.get()->isUTF16());
    CHECK(lat1.get()->equals(*utf1.get()));
    CHECK(utf1.get()->equals(*lat1.get()));
}

TEST_CASE("string ordering is by UTF-16 code unit, and a prefix comes first") {
    // ECMA-262 7.2.13 IsStringLessThan, which is what 13.10.1 step 3 uses to
    // compare two strings without converting either. Code units, not
    // characters and not a collation: `localeCompare` is deliberately
    // unimplemented and this must never become it.
    Heap heap;
    ShadowStackFrame frame;

    auto s = [&](const char* text) {
        return StringHeader::createFromUTF8(heap, text);
    };
    Rooted<StringHeader*> a(s("a"));
    Rooted<StringHeader*> b(s("b"));
    CHECK(a.get()->lessThan(*b.get()));
    CHECK_FALSE(b.get()->lessThan(*a.get()));

    // Equal strings are not less than each other — the case that makes `<=`
    // differ from `<`, and the reason this is not a three-way compare.
    Rooted<StringHeader*> a2(s("a"));
    CHECK_FALSE(a.get()->lessThan(*a2.get()));
    CHECK_FALSE(a.get()->lessThan(*a.get()));

    // A shared prefix: the first differing code unit decides, whatever
    // follows it.
    Rooted<StringHeader*> abc(s("abc"));
    Rooted<StringHeader*> abd(s("abd"));
    CHECK(abc.get()->lessThan(*abd.get()));
    CHECK_FALSE(abd.get()->lessThan(*abc.get()));

    // 7.2.13 step 3: a prefix is less than what extends it, and the empty
    // string is a prefix of everything.
    Rooted<StringHeader*> apple(s("apple"));
    Rooted<StringHeader*> apples(s("apples"));
    Rooted<StringHeader*> empty(s(""));
    CHECK(apple.get()->lessThan(*apples.get()));
    CHECK_FALSE(apples.get()->lessThan(*apple.get()));
    CHECK(empty.get()->lessThan(*apple.get()));
    CHECK_FALSE(empty.get()->lessThan(*empty.get()));

    // Mixed case: 0x5A precedes 0x61, so every uppercase letter sorts before
    // every lowercase one. A locale-aware comparison would answer otherwise,
    // and that is the whole point of pinning it.
    Rooted<StringHeader*> upperZ(s("Z"));
    Rooted<StringHeader*> lowerA(s("a"));
    CHECK(upperZ.get()->lessThan(*lowerA.get()));
    CHECK_FALSE(lowerA.get()->lessThan(*upperZ.get()));

    // Digits are code units too: "2" is above "1", so "2" is NOT less than
    // "10" the way the number 2 is less than 10.
    Rooted<StringHeader*> two(s("2"));
    Rooted<StringHeader*> ten(s("10"));
    CHECK_FALSE(two.get()->lessThan(*ten.get()));
    CHECK(ten.get()->lessThan(*two.get()));

    // A latin1 byte at or above 0x80 is a code unit above 0x7F, and `char` is
    // signed here — a memcmp would order this the other way round.
    const uint16_t highUnit[] = {0x00E9};  // é
    Rooted<StringHeader*> eacute(StringHeader::createUTF16(heap, highUnit, 1));
    CHECK(lowerA.get()->lessThan(*eacute.get()));
    CHECK_FALSE(eacute.get()->lessThan(*lowerA.get()));

    // The two representations answer the same question: a UTF-16 string and a
    // latin1 one with the same code units compare as equals.
    const uint16_t abcUnits[] = {'a', 'b', 'c'};
    Rooted<StringHeader*> abcWide(StringHeader::createUTF16(heap, abcUnits, 3));
    REQUIRE(abcWide.get()->isUTF16());
    CHECK_FALSE(abc.get()->lessThan(*abcWide.get()));
    CHECK_FALSE(abcWide.get()->lessThan(*abc.get()));
    CHECK(abcWide.get()->lessThan(*abd.get()));
}

TEST_CASE("hash computation and caching") {
    Heap heap;

    StringHeader* lat = StringHeader::createFromUTF8(heap, "HashMe");
    CHECK((lat->flags & StringHeader::kHasHashFlag) == 0);

    uint32_t h1 = lat->hash();
    CHECK((lat->flags & StringHeader::kHasHashFlag) != 0);

    uint32_t h2 = lat->hash();
    CHECK(h1 == h2);

    uint16_t u16_buf[] = {'H', 'a', 's', 'h', 'M', 'e'};
    StringHeader* utf = StringHeader::createUTF16(heap, u16_buf, 6);
    uint32_t h3 = utf->hash();
    CHECK(h1 == h3);
}

TEST_CASE("string concatenation") {
    Heap heap;
    ShadowStackFrame frame;

    Rooted<Value> rA(heap, Value::fromString(StringHeader::createFromUTF8(heap, "Hello")));
    Rooted<Value> rB(heap, Value::fromString(StringHeader::createFromUTF8(heap, " World")));

    Value concatRes1 = StringHeader::concat(heap, rA, rB);
    CHECK(concatRes1.isString());

    StringHeader* sRes1 = concatRes1.asString<StringHeader>();
    CHECK(sRes1->isLatin1());
    CHECK(sRes1->length == 11);
    CHECK(sRes1->charCodeAt(0) == 'H');
    CHECK(sRes1->charCodeAt(5) == ' ');
    CHECK(sRes1->charCodeAt(6) == 'W');

    Rooted<Value> rEmoji(heap, Value::fromString(StringHeader::createFromUTF8(heap, " 🌍")));
    Value concatRes2 = StringHeader::concat(heap, rA, rEmoji);
    CHECK(concatRes2.isString());

    StringHeader* sRes2 = concatRes2.asString<StringHeader>();
    CHECK(sRes2->isUTF16());
    CHECK(sRes2->length == 8);
    CHECK(sRes2->charCodeAt(5) == ' ');
    CHECK(sRes2->charCodeAt(6) == 0xD83C);
    CHECK(sRes2->charCodeAt(7) == 0xDF0D);
}

static Value callSlice(Value str, Value start = Value::fromUndefined(), Value end = Value::fromUndefined()) {
    uint64_t argv[2];
    uint32_t argc = 0;
    if (!start.isUndefined()) {
        argv[argc++] = start.rawBits();
        if (!end.isUndefined()) {
            argv[argc++] = end.rawBits();
        }
    }
    return Value(runtime::stringSlice(0, str.rawBits(), argc, argv));
}

static Value callSubstring(Value str, Value start = Value::fromUndefined(), Value end = Value::fromUndefined()) {
    uint64_t argv[2];
    uint32_t argc = 0;
    if (!start.isUndefined()) {
        argv[argc++] = start.rawBits();
        if (!end.isUndefined()) {
            argv[argc++] = end.rawBits();
        }
    }
    return Value(runtime::stringSubstring(0, str.rawBits(), argc, argv));
}

static Value callSubstr(Value str, Value start = Value::fromUndefined(), Value length = Value::fromUndefined()) {
    uint64_t argv[2];
    uint32_t argc = 0;
    if (!start.isUndefined()) {
        argv[argc++] = start.rawBits();
        if (!length.isUndefined()) {
            argv[argc++] = length.rawBits();
        }
    }
    return Value(runtime::stringSubstr(0, str.rawBits(), argc, argv));
}

TEST_CASE("fast direct string slicing - Latin-1 strings (ASCII)") {
    Heap heap;
    ShadowStackFrame frame;

    Rooted<Value> rStr(heap, Value::fromString(StringHeader::createFromUTF8(heap, "Hello, World!")));
    Value str = rStr.get();

    // slice(7, 12) -> "World"
    Value sl1 = callSlice(str, Value::fromDouble(7), Value::fromDouble(12));
    CHECK(sl1.isString());
    StringHeader* h1 = sl1.asString<StringHeader>();
    CHECK(h1->isLatin1());
    CHECK(!h1->isUTF16());
    CHECK(h1->length == 5);
    CHECK(std::string(h1->latin1Data(), h1->length) == "World");

    // slice(7) -> "World!"
    Value sl2 = callSlice(str, Value::fromDouble(7));
    StringHeader* h2 = sl2.asString<StringHeader>();
    CHECK(h2->isLatin1());
    CHECK(h2->length == 6);
    CHECK(std::string(h2->latin1Data(), h2->length) == "World!");

    // slice(-6, -1) -> "World"
    Value sl3 = callSlice(str, Value::fromDouble(-6), Value::fromDouble(-1));
    StringHeader* h3 = sl3.asString<StringHeader>();
    CHECK(h3->isLatin1());
    CHECK(std::string(h3->latin1Data(), h3->length) == "World");

    // slice(-100, 5) -> "Hello"
    Value sl4 = callSlice(str, Value::fromDouble(-100), Value::fromDouble(5));
    StringHeader* h4 = sl4.asString<StringHeader>();
    CHECK(h4->isLatin1());
    CHECK(std::string(h4->latin1Data(), h4->length) == "Hello");

    // slice(5, 2) -> ""
    Value sl5 = callSlice(str, Value::fromDouble(5), Value::fromDouble(2));
    StringHeader* h5 = sl5.asString<StringHeader>();
    CHECK(h5->isLatin1());
    CHECK(h5->length == 0);

    // substring(7, 12) -> "World"
    Value sub1 = callSubstring(str, Value::fromDouble(7), Value::fromDouble(12));
    StringHeader* sh1 = sub1.asString<StringHeader>();
    CHECK(sh1->isLatin1());
    CHECK(std::string(sh1->latin1Data(), sh1->length) == "World");

    // substring(12, 7) -> "World" (swaps arguments)
    Value sub2 = callSubstring(str, Value::fromDouble(12), Value::fromDouble(7));
    StringHeader* sh2 = sub2.asString<StringHeader>();
    CHECK(sh2->isLatin1());
    CHECK(std::string(sh2->latin1Data(), sh2->length) == "World");

    // substring(-5, 5) -> "Hello" (clamps negative to 0)
    Value sub3 = callSubstring(str, Value::fromDouble(-5), Value::fromDouble(5));
    StringHeader* sh3 = sub3.asString<StringHeader>();
    CHECK(sh3->isLatin1());
    CHECK(std::string(sh3->latin1Data(), sh3->length) == "Hello");

    // substr(7, 5) -> "World"
    Value sstr1 = callSubstr(str, Value::fromDouble(7), Value::fromDouble(5));
    StringHeader* ssh1 = sstr1.asString<StringHeader>();
    CHECK(ssh1->isLatin1());
    CHECK(std::string(ssh1->latin1Data(), ssh1->length) == "World");

    // substr(-6, 5) -> "World"
    Value sstr2 = callSubstr(str, Value::fromDouble(-6), Value::fromDouble(5));
    StringHeader* ssh2 = sstr2.asString<StringHeader>();
    CHECK(ssh2->isLatin1());
    CHECK(std::string(ssh2->latin1Data(), ssh2->length) == "World");

    // substr(7, 0) -> ""
    Value sstr3 = callSubstr(str, Value::fromDouble(7), Value::fromDouble(0));
    StringHeader* ssh3 = sstr3.asString<StringHeader>();
    CHECK(ssh3->length == 0);
}

TEST_CASE("fast direct string slicing - UTF-16 strings (BMP non-ASCII and surrogate pairs)") {
    Heap heap;
    ShadowStackFrame frame;

    // BMP non-ASCII: "Hello 世界!"
    Rooted<Value> rBmp(heap, Value::fromString(StringHeader::createFromUTF8(heap, "Hello 世界!")));
    Value bmpStr = rBmp.get();
    StringHeader* bmpH = bmpStr.asString<StringHeader>();
    REQUIRE(bmpH->isUTF16());
    REQUIRE(bmpH->length == 9);

    // Slicing UTF-16 non-ASCII range [6, 8) -> "世界"
    Value slBmp1 = callSlice(bmpStr, Value::fromDouble(6), Value::fromDouble(8));
    StringHeader* hBmp1 = slBmp1.asString<StringHeader>();
    CHECK(hBmp1->isUTF16());
    CHECK(!hBmp1->isLatin1());
    CHECK(hBmp1->length == 2);
    CHECK(hBmp1->charCodeAt(0) == 0x4E16);
    CHECK(hBmp1->charCodeAt(1) == 0x754C);

    // Slicing ASCII range [0, 5) from UTF-16 string -> "Hello" (MUST promote to Latin-1!)
    Value slBmp2 = callSlice(bmpStr, Value::fromDouble(0), Value::fromDouble(5));
    StringHeader* hBmp2 = slBmp2.asString<StringHeader>();
    CHECK(hBmp2->isLatin1());
    CHECK(!hBmp2->isUTF16());
    CHECK(hBmp2->length == 5);
    CHECK(std::string(hBmp2->latin1Data(), hBmp2->length) == "Hello");

    // Slicing mixed range [6, 9) -> "世界!" (contains > 0xFF and <= 0xFF, so UTF-16)
    Value slBmp3 = callSlice(bmpStr, Value::fromDouble(6), Value::fromDouble(9));
    StringHeader* hBmp3 = slBmp3.asString<StringHeader>();
    CHECK(hBmp3->isUTF16());
    CHECK(hBmp3->length == 3);
    CHECK(hBmp3->charCodeAt(2) == '!');

    // substring swap on UTF-16: substring(8, 6) -> "世界"
    Value subBmp1 = callSubstring(bmpStr, Value::fromDouble(8), Value::fromDouble(6));
    StringHeader* hSubBmp1 = subBmp1.asString<StringHeader>();
    CHECK(hSubBmp1->isUTF16());
    CHECK(hSubBmp1->length == 2);
    CHECK(hSubBmp1->charCodeAt(0) == 0x4E16);

    // substring on ASCII range: substring(5, 0) -> "Hello" (swapped, Latin-1)
    Value subBmp2 = callSubstring(bmpStr, Value::fromDouble(5), Value::fromDouble(0));
    StringHeader* hSubBmp2 = subBmp2.asString<StringHeader>();
    CHECK(hSubBmp2->isLatin1());
    CHECK(hSubBmp2->length == 5);
    CHECK(std::string(hSubBmp2->latin1Data(), hSubBmp2->length) == "Hello");

    // substr on UTF-16: substr(6, 2) -> "世界"
    Value sstrBmp = callSubstr(bmpStr, Value::fromDouble(6), Value::fromDouble(2));
    StringHeader* hSstrBmp = sstrBmp.asString<StringHeader>();
    CHECK(hSstrBmp->isUTF16());
    CHECK(hSstrBmp->length == 2);

    // Surrogate pairs: "Hi 🌍 there"
    Rooted<Value> rEmoji(heap, Value::fromString(StringHeader::createFromUTF8(heap, "Hi 🌍 there")));
    Value emojiStr = rEmoji.get();
    StringHeader* emojiH = emojiStr.asString<StringHeader>();
    REQUIRE(emojiH->isUTF16());
    REQUIRE(emojiH->length == 11);
    CHECK(emojiH->charCodeAt(3) == 0xD83C);
    CHECK(emojiH->charCodeAt(4) == 0xDF0D);

    // Slicing the surrogate pair [3, 5) -> "🌍"
    Value slEmoji1 = callSlice(emojiStr, Value::fromDouble(3), Value::fromDouble(5));
    StringHeader* hEmoji1 = slEmoji1.asString<StringHeader>();
    CHECK(hEmoji1->isUTF16());
    CHECK(hEmoji1->length == 2);
    CHECK(hEmoji1->charCodeAt(0) == 0xD83C);
    CHECK(hEmoji1->charCodeAt(1) == 0xDF0D);

    // Slicing ASCII prefix [0, 2) from emoji string -> "Hi" (Latin-1)
    Value slEmoji2 = callSlice(emojiStr, Value::fromDouble(0), Value::fromDouble(2));
    StringHeader* hEmoji2 = slEmoji2.asString<StringHeader>();
    CHECK(hEmoji2->isLatin1());
    CHECK(!hEmoji2->isUTF16());
    CHECK(hEmoji2->length == 2);
    CHECK(std::string(hEmoji2->latin1Data(), hEmoji2->length) == "Hi");

    // Slicing ASCII suffix [5, 11) from emoji string -> " there" (Latin-1)
    Value slEmoji3 = callSlice(emojiStr, Value::fromDouble(5), Value::fromDouble(11));
    StringHeader* hEmoji3 = slEmoji3.asString<StringHeader>();
    CHECK(hEmoji3->isLatin1());
    CHECK(!hEmoji3->isUTF16());
    CHECK(hEmoji3->length == 6);
    CHECK(std::string(hEmoji3->latin1Data(), hEmoji3->length) == " there");

    // Splitting a surrogate pair [3, 4) -> leading surrogate unit 0xD83C (UTF-16)
    Value slEmoji4 = callSlice(emojiStr, Value::fromDouble(3), Value::fromDouble(4));
    StringHeader* hEmoji4 = slEmoji4.asString<StringHeader>();
    CHECK(hEmoji4->isUTF16());
    CHECK(hEmoji4->length == 1);
    CHECK(hEmoji4->charCodeAt(0) == 0xD83C);
}
