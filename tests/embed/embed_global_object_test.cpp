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

TEST_CASE("a host global belongs to the realm it was registered in") {
    ShadowStackFrame frame;

    embed::registerGlobal("defaultOnlyGlobal", embed::fromDouble(9.0));
    embed::Persistent defGlobal{embed::globalValue("globalThis").value};

    // A realm created later starts with none of the default realm's: what a
    // host installs is mutable, and a realm reaching another's could change it.
    embed::Realm* fresh = embed::createRealm();
    REQUIRE(fresh != nullptr);
    embed::Persistent freshGlobal{embed::getRealmGlobal(fresh)};
    CHECK(embed::isUndefined(embed::getProperty(freshGlobal.get(), "defaultOnlyGlobal")));
    CHECK(!inGlobal(freshGlobal.get(), "defaultOnlyGlobal"));
    {
        embed::RealmScope scope(fresh);
        CHECK(!embed::globalValue("defaultOnlyGlobal").found);
        CHECK(!embed::hasHostGlobal("defaultOnlyGlobal"));
        CHECK(!listed(embed::hostGlobalNames(), "defaultOnlyGlobal"));

        // A registration in the fresh realm lands there and nowhere else.
        embed::registerGlobal("freshOnlyGlobal", embed::fromDouble(5.0));
        CHECK(embed::toDouble(embed::globalValue("freshOnlyGlobal").value) == 5.0);
        CHECK(embed::toDouble(embed::getProperty(freshGlobal.get(), "freshOnlyGlobal")) == 5.0);

        // One name, two realms, two values.
        embed::registerGlobal("perRealmGlobal", embed::fromDouble(2.0));
    }
    embed::registerGlobal("perRealmGlobal", embed::fromDouble(1.0));
    CHECK(!embed::globalValue("freshOnlyGlobal").found);
    CHECK(embed::isUndefined(embed::getProperty(defGlobal.get(), "freshOnlyGlobal")));
    CHECK(embed::toDouble(embed::globalValue("perRealmGlobal").value) == 1.0);
    CHECK(embed::toDouble(embed::getProperty(freshGlobal.get(), "perRealmGlobal")) == 2.0);
    {
        embed::RealmScope scope(fresh);
        CHECK(embed::toDouble(embed::globalValue("perRealmGlobal").value) == 2.0);
    }
    CHECK(embed::toDouble(embed::globalValue("defaultOnlyGlobal").value) == 9.0);

    // The fresh realm's values survive a collection: the realm roots them.
    runtime::rtHeap().collect();
    {
        embed::RealmScope scope(fresh);
        CHECK(embed::toDouble(embed::globalValue("freshOnlyGlobal").value) == 5.0);
    }

    embed::destroyRealm(fresh);
}

TEST_CASE("a lazy host global is listed at once and built at its first read") {
    ShadowStackFrame frame;
    embed::Persistent glob{embed::globalValue("globalThis").value};
    REQUIRE(embed::isObject(glob.get()));

    // Two names, one init: forcing either builds both.
    static int inits = 0;
    inits = 0;
    auto init = [] {
        static bool done = false;
        if (done) return;
        done = true;
        ++inits;
        embed::registerGlobal("lazyHostA", embed::fromDouble(1.0));
        embed::registerGlobal("lazyHostB", embed::fromDouble(2.0));
    };
    embed::registerLazyGlobal("lazyHostA", init);
    embed::registerLazyGlobal("lazyHostB", init);

    // Listed, and present on globalThis, without running the init.
    CHECK(embed::hasHostGlobal("lazyHostA"));
    const std::vector<std::string> names = embed::hostGlobalNames();
    CHECK(listed(names, "lazyHostA"));
    CHECK(listed(names, "lazyHostB"));
    CHECK(inGlobal(glob.get(), "lazyHostA"));
    CHECK(listed(globalOwnNames(), "lazyHostB"));
    CHECK(inits == 0);

    // The property read forces it.
    CHECK(embed::toDouble(embed::getProperty(glob.get(), "lazyHostB")) == 2.0);
    CHECK(inits == 1);
    // The other name was registered by the same init: a value, not a second run.
    CHECK(embed::toDouble(embed::globalValue("lazyHostA").value) == 1.0);
    CHECK(embed::toDouble(embed::getProperty(glob.get(), "lazyHostA")) == 1.0);
    CHECK(inits == 1);

    // A realm created afterwards has none of them.
    embed::Realm* fresh = embed::createRealm();
    REQUIRE(fresh != nullptr);
    embed::Persistent freshGlobal{embed::getRealmGlobal(fresh)};
    CHECK(embed::isUndefined(embed::getProperty(freshGlobal.get(), "lazyHostA")));
    embed::destroyRealm(fresh);
}

TEST_CASE("a lazy host global is forced in the realm it was registered in") {
    ShadowStackFrame frame;
    static int inits = 0;
    embed::registerLazyGlobal("lazyHostC", [] {
        ++inits;
        embed::registerGlobal("lazyHostC", embed::fromUtf8("built"));
    });
    embed::Persistent defGlobal{embed::globalValue("globalThis").value};
    embed::Realm* fresh = embed::createRealm();
    REQUIRE(fresh != nullptr);
    embed::Persistent freshGlobal{embed::getRealmGlobal(fresh)};
    CHECK(!inGlobal(freshGlobal.get(), "lazyHostC"));
    CHECK(inits == 0);
    {
        // Read from the other realm: the accessor forces the name, and the
        // init registers it, in the realm that owns the accessor.
        embed::RealmScope scope(fresh);
        CHECK(embed::toUtf8(embed::getProperty(defGlobal.get(), "lazyHostC")) == "built");
        CHECK(inits == 1);
        CHECK(!embed::globalValue("lazyHostC").found);
    }
    CHECK(embed::toUtf8(embed::globalValue("lazyHostC").value) == "built");
    CHECK(embed::toUtf8(embed::getProperty(defGlobal.get(), "lazyHostC")) == "built");
    CHECK(inits == 1);
    CHECK(!inGlobal(freshGlobal.get(), "lazyHostC"));
    embed::destroyRealm(fresh);

    // An init that registers nothing leaves undefined.
    embed::registerLazyGlobal("lazyHostEmpty", [] {});
    CHECK(embed::globalValue("lazyHostEmpty").found);
    CHECK(embed::isUndefined(embed::globalValue("lazyHostEmpty").value));
}
