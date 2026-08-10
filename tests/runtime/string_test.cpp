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
