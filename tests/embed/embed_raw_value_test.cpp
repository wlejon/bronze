// A Value held raw in a C++ local, across calls that allocate: the heap keeps
// in place every young object the thread's stack and registers name (brass
// docs/gc_contract.md, "Roots"), so the local still names its object. Run
// under BRONZE_GC_STRESS (embed-gc-stress), where each allocation collects,
// these are the patterns that read freed memory when collections moved what
// the stack named.

#include <doctest/doctest.h>

#include <cstdlib>
#include <string>
#include "embed/embed.h"

using namespace bronze;

namespace {
bool pinning_on() {
    const char* v = std::getenv("BRASS_GC_PIN_STACK");
    return v && *v && std::string(v) != "0";
}
} // namespace

TEST_CASE("a raw receiver beside an allocating argument stays current") {
    if (!pinning_on()) return;
    embed::Value obj = embed::createObject();
    for (int i = 0; i < 64; ++i) {
        const std::string key = "k" + std::to_string(i);
        // The receiver is read, then the argument allocates (or the other
        // way round): either order is fine now.
        embed::setProperty(obj, key, embed::fromUtf8("v" + std::to_string(i)));
    }
    for (int i = 0; i < 64; ++i) {
        const std::string key = "k" + std::to_string(i);
        CHECK(embed::toUtf8(embed::getProperty(obj, key)) == "v" + std::to_string(i));
    }
}

TEST_CASE("raw values held across getProperty and other allocating calls stay current") {
    if (!pinning_on()) return;
    embed::Value outer = embed::createObject();
    embed::Value inner = embed::createObject();
    embed::Value text = embed::fromUtf8("held across allocation");
    embed::setProperty(inner, "text", text);
    embed::setProperty(outer, "inner", inner);
    for (int i = 0; i < 200; ++i) {
        (void)embed::fromUtf8("garbage " + std::to_string(i));
        embed::Value got = embed::getProperty(outer, "inner");
        CHECK(embed::isObject(got));
        CHECK(embed::toUtf8(embed::getProperty(inner, "text")) == "held across allocation");
        CHECK(embed::toUtf8(text) == "held across allocation");
        embed::setElement(outer, static_cast<uint32_t>(i % 8), embed::fromUtf8(std::to_string(i)));
    }
    embed::collectGarbage();
    CHECK(embed::toUtf8(embed::getProperty(embed::getProperty(outer, "inner"), "text")) ==
          "held across allocation");
}
