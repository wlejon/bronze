#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "embed/embed.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/rt_state.h"
#include "runtime/value.h"

using namespace bronze;

TEST_CASE("realm creation and default realm basics") {
    embed::Realm* def = embed::currentRealm();
    CHECK(def != nullptr);

    embed::Value defGlobal = embed::getRealmGlobal(def);
    CHECK(embed::isObject(defGlobal));

    // Current realm global matches globalThis
    embed::GlobalValue gt = embed::globalValue("globalThis");
    CHECK(gt.found);
    CHECK(gt.value.rawBits() == defGlobal.rawBits());

    // Creating new realms produces distinct globals
    embed::Realm* realmA = embed::createRealm();
    embed::Realm* realmB = embed::createRealm();
    CHECK(realmA != nullptr);
    CHECK(realmB != nullptr);
    CHECK(realmA != def);
    CHECK(realmB != def);
    CHECK(realmA != realmB);

    embed::Value globA = embed::getRealmGlobal(realmA);
    embed::Value globB = embed::getRealmGlobal(realmB);
    CHECK(embed::isObject(globA));
    CHECK(embed::isObject(globB));
    CHECK(globA.rawBits() != globB.rawBits());
    CHECK(globA.rawBits() != defGlobal.rawBits());

    // Each realm has globalThis pointing to itself
    embed::Value selfA = embed::getProperty(globA, "globalThis");
    embed::Value selfB = embed::getProperty(globB, "globalThis");
    CHECK(selfA.rawBits() == globA.rawBits());
    CHECK(selfB.rawBits() == globB.rawBits());

    // Builtins are populated on both realms
    embed::Value mathA = embed::getProperty(globA, "Math");
    embed::Value mathB = embed::getProperty(globB, "Math");
    CHECK(embed::isObject(mathA));
    CHECK(embed::isObject(mathB));

    embed::destroyRealm(realmA);
    embed::destroyRealm(realmB);
}

TEST_CASE("realm stack operations and RealmScope") {
    embed::Realm* def = embed::currentRealm();
    embed::Realm* realmA = embed::createRealm();
    embed::Realm* realmB = embed::createRealm();

    CHECK(embed::currentRealm() == def);

    embed::enterRealm(realmA);
    CHECK(embed::currentRealm() == realmA);
    CHECK(embed::getRealmGlobal().rawBits() == embed::getRealmGlobal(realmA).rawBits());

    embed::enterRealm(realmB);
    CHECK(embed::currentRealm() == realmB);
    CHECK(embed::getRealmGlobal().rawBits() == embed::getRealmGlobal(realmB).rawBits());

    embed::exitRealm();
    CHECK(embed::currentRealm() == realmA);

    embed::exitRealm();
    CHECK(embed::currentRealm() == def);

    // Test RAII RealmScope
    {
        embed::RealmScope scopeA(realmA);
        CHECK(embed::currentRealm() == realmA);
        {
            embed::RealmScope scopeB(realmB);
            CHECK(embed::currentRealm() == realmB);
        }
        CHECK(embed::currentRealm() == realmA);
    }
    CHECK(embed::currentRealm() == def);

    embed::destroyRealm(realmA);
    embed::destroyRealm(realmB);
}

TEST_CASE("global variable isolation across realms") {
    embed::Realm* realmA = embed::createRealm();
    embed::Realm* realmB = embed::createRealm();
    embed::Value globA = embed::getRealmGlobal(realmA);
    embed::Value globB = embed::getRealmGlobal(realmB);

    // Set variable on Realm A
    embed::setProperty(globA, "x", embed::fromDouble(42.0));
    embed::setProperty(globA, "greeting", embed::fromUtf8("hello from A"));

    // Realm B should not see Realm A's variables
    CHECK(embed::isUndefined(embed::getProperty(globB, "x")));
    CHECK(embed::isUndefined(embed::getProperty(globB, "greeting")));

    // Set variable on Realm B with different value
    embed::setProperty(globB, "x", embed::fromDouble(100.0));
    embed::setProperty(globB, "greeting", embed::fromUtf8("hello from B"));

    // Verify isolation when inspecting globals directly
    CHECK(embed::toDouble(embed::getProperty(globA, "x")) == 42.0);
    CHECK(embed::toUtf8(embed::getProperty(globA, "greeting")) == "hello from A");
    CHECK(embed::toDouble(embed::getProperty(globB, "x")) == 100.0);
    CHECK(embed::toUtf8(embed::getProperty(globB, "greeting")) == "hello from B");

    // Verify isolation via active realm lookups (globalValue)
    {
        embed::RealmScope scopeA(realmA);
        embed::GlobalValue gvX = embed::globalValue("x");
        CHECK(gvX.found);
        CHECK(embed::toDouble(gvX.value) == 42.0);

        embed::GlobalValue gvGreet = embed::globalValue("greeting");
        CHECK(gvGreet.found);
        CHECK(embed::toUtf8(gvGreet.value) == "hello from A");
    }

    {
        embed::RealmScope scopeB(realmB);
        embed::GlobalValue gvX = embed::globalValue("x");
        CHECK(gvX.found);
        CHECK(embed::toDouble(gvX.value) == 100.0);

        embed::GlobalValue gvGreet = embed::globalValue("greeting");
        CHECK(gvGreet.found);
        CHECK(embed::toUtf8(gvGreet.value) == "hello from B");
    }

    // Default realm does not have x or greeting
    CHECK(!embed::globalValue("x").found);
    CHECK(!embed::globalValue("greeting").found);

    embed::destroyRealm(realmA);
    embed::destroyRealm(realmB);
}

TEST_CASE("realm GC root tracing preserves global objects under collection") {
    embed::Realm* realmA = embed::createRealm();
    embed::Realm* realmB = embed::createRealm();

    {
        embed::RealmScope scopeA(realmA);
        embed::setProperty(embed::getRealmGlobal(realmA), "trackedString",
                           embed::fromUtf8("survives moving collection in realm A"));
        embed::setProperty(embed::getRealmGlobal(realmA), "trackedNumber", embed::fromDouble(777.0));
    }
    {
        embed::RealmScope scopeB(realmB);
        embed::setProperty(embed::getRealmGlobal(realmB), "trackedString",
                           embed::fromUtf8("survives moving collection in realm B"));
        embed::setProperty(embed::getRealmGlobal(realmB), "trackedNumber", embed::fromDouble(888.0));
    }

    // Force moving GC collection
    runtime::rtHeap().collect();

    // Verify properties still valid after collection
    {
        embed::RealmScope scopeA(realmA);
        embed::Value strVal = embed::getProperty(embed::getRealmGlobal(realmA), "trackedString");
        embed::Value numVal = embed::getProperty(embed::getRealmGlobal(realmA), "trackedNumber");
        CHECK(embed::isString(strVal));
        CHECK(embed::toUtf8(strVal) == "survives moving collection in realm A");
        CHECK(embed::toDouble(numVal) == 777.0);
    }
    {
        embed::RealmScope scopeB(realmB);
        embed::Value strVal = embed::getProperty(embed::getRealmGlobal(realmB), "trackedString");
        embed::Value numVal = embed::getProperty(embed::getRealmGlobal(realmB), "trackedNumber");
        CHECK(embed::isString(strVal));
        CHECK(embed::toUtf8(strVal) == "survives moving collection in realm B");
        CHECK(embed::toDouble(numVal) == 888.0);
    }

    embed::destroyRealm(realmA);
    embed::destroyRealm(realmB);
}

TEST_CASE("destroying an active realm safely restores previous realm") {
    embed::Realm* def = embed::currentRealm();
    embed::Realm* realmA = embed::createRealm();
    embed::Realm* realmB = embed::createRealm();

    embed::enterRealm(realmA);
    embed::enterRealm(realmB);
    CHECK(embed::currentRealm() == realmB);

    // Destroy active realm
    embed::destroyRealm(realmB);
    CHECK(embed::currentRealm() == realmA);

    embed::destroyRealm(realmA);
    CHECK(embed::currentRealm() == def);
}
