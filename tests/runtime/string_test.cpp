#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "runtime/gc.h"
#include "runtime/heap.h"
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
