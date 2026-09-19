// A host global must be reachable BOTH ways: by the bare name a compiled read
// resolves through the registry, and as an own property of `globalThis`.
//
// It used to be only the first. `Realm::initGlobalObject` copied the registry
// once, when the realm was built, and `rtRegisterHostGlobal` never touched a
// realm again — so a host that registers anything after its first look at
// `globalThis` (which is every host, because looking is what creates the
// default realm) had those names invisible to `globalThis[name]`,
// `name in globalThis`, `Object.getOwnPropertyNames(globalThis)` and every
// reflective walk built on them. Feature detection is exactly that walk.

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "embed/embed.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/value.h"

using namespace bronze;

namespace {

// The own string keys of `globalThis` — the list `Object.getOwnPropertyNames`
// walks, read through the same function the member does.
std::vector<std::string> globalOwnNames() {
    embed::Value glob = embed::globalValue("globalThis").value;
    std::vector<std::string> out;
    for (StringHeader* key :
         runtime::rtOwnStringKeysOrdered(glob.asObject<ObjectHeader>(),
                                         /*enumerableOnly=*/false)) {
        out.push_back(runtime::rtUtf8Chars(key));
    }
    return out;
}

// `name in globalThis`, through the property path rather than the registry.
bool inGlobal(embed::Value glob, const char* name) {
    ShadowStackFrame frame;
    // Rooted before the string is minted: making one allocates, and a raw
    // object handle taken across an allocation is what the gc-stress run
    // exists to catch. The key is the FIRST argument (rt_operator.cpp).
    Rooted<Value> obj{glob};
    Rooted<Value> key{runtime::rtMakeString(name)};
    return bronze_has_property(key.get().rawBits(), obj.get().rawBits());
}

bool listed(const std::vector<std::string>& names, const std::string& want) {
    return std::find(names.begin(), names.end(), want) != names.end();
}

}  // namespace

TEST_CASE("a host global registered after the realm exists is an own property of globalThis") {
    ShadowStackFrame frame;

    // Force the default realm into being FIRST, so the registration below is
    // the late one that used to be lost. This is the sequence every host hits.
    embed::Persistent glob{embed::globalValue("globalThis").value};
    REQUIRE(embed::isObject(glob.get()));

    embed::registerGlobal("lateHostGlobal", embed::fromUtf8("present"));

    // By computed key.
    embed::Value byKey = embed::getProperty(glob.get(), "lateHostGlobal");
    CHECK(embed::isString(byKey));
    CHECK(embed::toUtf8(byKey) == "present");

    // By `in`, which is the property path rather than the registry.
    CHECK(inGlobal(glob.get(), "lateHostGlobal"));

    // And in the listing a reflective walk enumerates.
    CHECK(listed(globalOwnNames(), "lateHostGlobal"));

    // A heap value on purpose: the property holds a real slot, so it must
    // follow the string when the collector moves it.
    runtime::rtHeap().collect();
    embed::Value afterGc = embed::getProperty(glob.get(), "lateHostGlobal");
    CHECK(embed::toUtf8(afterGc) == "present");
}

TEST_CASE("re-registering a host global updates the globalThis property too") {
    ShadowStackFrame frame;
    embed::Persistent glob{embed::globalValue("globalThis").value};

    embed::registerGlobal("replacedHostGlobal", embed::fromDouble(1.0));
    CHECK(embed::toDouble(embed::getProperty(glob.get(), "replacedHostGlobal")) == 1.0);

    embed::registerGlobal("replacedHostGlobal", embed::fromDouble(2.0));
    CHECK(embed::toDouble(embed::getProperty(glob.get(), "replacedHostGlobal")) == 2.0);

    // One property, not two: the registry names each global once and so does
    // the object.
    const std::vector<std::string> names = globalOwnNames();
    CHECK(std::count(names.begin(), names.end(), std::string("replacedHostGlobal")) == 1);
}

TEST_CASE("a host global named after a builtin is not written onto globalThis") {
    ShadowStackFrame frame;
    embed::Persistent glob{embed::globalValue("globalThis").value};

    // The bare-name ladder asks the builtins FIRST and deliberately: compiled
    // code was optimized against the real `Math`. So the property must keep
    // agreeing with the bare name rather than diverging from it.
    embed::registerGlobal("Math", embed::fromDouble(13.0));
    embed::Value math = embed::getProperty(glob.get(), "Math");
    CHECK(embed::isObject(math));
    CHECK(embed::toDouble(embed::getProperty(math, "E")) > 2.7);

    // `performance` is the one name a host may shadow (rt_state.cpp says why),
    // and there the property follows the host. An OBJECT rather than a number,
    // because the cases in this binary share one registry and the case in
    // embed_test.cpp asserts the builtin is an object before it registers its
    // own — an order-independent suite is a property this file owes too.
    embed::Persistent clock{embed::createObject()};
    embed::setProperty(clock.get(), "hostClockMarker", embed::fromDouble(4242.0));
    embed::registerGlobal("performance", clock.get());
    embed::Value viaGlobal = embed::getProperty(glob.get(), "performance");
    REQUIRE(embed::isObject(viaGlobal));
    CHECK(embed::toDouble(embed::getProperty(viaGlobal, "hostClockMarker")) == 4242.0);
}

TEST_CASE("a realm created later carries the whole registry, existing realms keep theirs") {
    ShadowStackFrame frame;

    embed::registerGlobal("sharedHostGlobal", embed::fromDouble(9.0));

    embed::Realm* fresh = embed::createRealm();
    REQUIRE(fresh != nullptr);
    embed::Persistent freshGlobal{embed::getRealmGlobal(fresh)};
    CHECK(embed::toDouble(embed::getProperty(freshGlobal.get(), "sharedHostGlobal")) == 9.0);

    // A registration after BOTH realms exist lands on both.
    embed::registerGlobal("everyRealmGlobal", embed::fromDouble(5.0));
    embed::Persistent defGlobal{embed::globalValue("globalThis").value};
    CHECK(embed::toDouble(embed::getProperty(defGlobal.get(), "everyRealmGlobal")) == 5.0);
    CHECK(embed::toDouble(embed::getProperty(freshGlobal.get(), "everyRealmGlobal")) == 5.0);

    embed::destroyRealm(fresh);
}
