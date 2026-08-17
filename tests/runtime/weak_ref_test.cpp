// `WeakRef` and `FinalizationRegistry` at the level their behaviour actually
// lives at: the collector.
//
// None of this can be an oracle case, and the reason is worth stating where the
// tests are. An oracle case's stdout is pinned byte-for-byte and every case runs
// both with and without `BRONZE_GC_STRESS=1` — so a program may only print
// things that are identical whether the collector ran a thousand times or never.
// "Does `deref` answer undefined after the last reference is dropped" is exactly
// not that: it is `false` in a run with no collection and `true` in a run with
// one, and both are correct. The specification goes further and permits a host
// never to call a cleanup callback at all.
//
// So the behaviour is pinned HERE, where C++ can call `rtHeap().collect()` and
// assert what the sweep did: which weak slots were forwarded, which were
// cleared, which cells were parked, and that a callback ran from a job rather
// than from inside the collection.

#include <doctest/doctest.h>

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"
#include "runtime/weak_ref.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

int g_cleanupCalls = 0;
double g_lastHeldNumber = 0.0;
std::string g_lastHeldText;

// The cleanup callback every case here registers. It records rather than
// asserts, because what a case wants to know is that it ran ONCE, from the job,
// with the held value the registration named.
uint64_t recordCleanup(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    (void)thisBits;
    ++g_cleanupCalls;
    const Value held = argc > 0 ? Value(argv[0]) : Value::fromUndefined();
    g_lastHeldNumber = held.isNumber() ? held.asNumber() : 0.0;
    g_lastHeldText.clear();
    if (held.isString()) g_lastHeldText = rtUtf8Chars(held.asString<StringHeader>());
    return Value::fromUndefined().rawBits();
}

// The weak tables are process-wide and every case in this binary shares them, so
// a case starts by settling whatever an earlier one left: clear the kept list,
// collect so every already-dead cell parks, drain those cleanup jobs, and only
// then zero the counters. Without this a case would be asserting about another
// case's garbage.
void quiesce() {
    rtClearKeptObjects();
    rtHeap().collect();
    while (rtFinalizationCleanupPending()) rtRunFinalizationCleanupJob();
    g_cleanupCalls = 0;
    g_lastHeldNumber = 0.0;
    g_lastHeldText.clear();
}

Value weakRefTargetSlot(Rooted<Value>& wr) {
    return wr.get().asObject<WeakRefHeader>()->target();
}

}  // namespace

TEST_CASE("a WeakRef's target survives every collection while something else holds it") {
    ShadowStackFrame frame;
    quiesce();

    Rooted<Value> target{Value(bronze_create_object())};
    Rooted<Value> wr{rtMakeWeakRef(target)};

    for (int i = 0; i < 3; ++i) rtHeap().collect();

    // Not merely non-undefined: the slot FOLLOWED the object. A sweep that
    // forwarded nothing would leave the old from-space address here, which
    // compares unequal to the root's forwarded one — so this one line is what
    // separates a working sweep from a stale pointer that happens to be
    // readable.
    CHECK(weakRefTargetSlot(wr).rawBits() == target.get().rawBits());
    CHECK(rtWeakRefDeref(wr.get()).rawBits() == target.get().rawBits());
}

TEST_CASE("the weak slot is FORWARDED across a collection, not merely left readable") {
    ShadowStackFrame frame;
    quiesce();

    Rooted<Value> target{Value(bronze_create_object())};
    Rooted<Value> wr{rtMakeWeakRef(target)};
    const uint64_t before = weakRefTargetSlot(wr).rawBits();

    rtHeap().collect();

    const uint64_t after = weakRefTargetSlot(wr).rawBits();
    // The semispaces are disjoint, so one collection cannot leave a surviving
    // object where it was.
    CHECK(before != after);
    CHECK(after == target.get().rawBits());
}

TEST_CASE("KeepDuringJob: a deref'd target outlives its last reference until the list clears") {
    ShadowStackFrame frame;
    quiesce();

    Rooted<Value> wr{Value::fromUndefined()};
    {
        Rooted<Value> target{Value(bronze_create_object())};
        wr.set(rtMakeWeakRef(target));
        // 26.1.1.1 step 4 and 26.1.3.2 both add the target to [[KeptAlive]].
        CHECK(rtKeptObjectCount() >= 1);
        CHECK(rtWeakRefDeref(wr.get()).rawBits() == target.get().rawBits());
    }

    // The target is out of every frame root now, and the ONLY thing left
    // referring to it is the kept-objects list. 9.13 says that is enough:
    // within one job a target that has been observed may not stop existing.
    rtHeap().collect();
    CHECK_FALSE(weakRefTargetSlot(wr).isUndefined());

    // ClearKeptObjects is the microtask checkpoint's, and this is what it buys:
    // the same collection that changed nothing a moment ago now clears the slot.
    rtClearKeptObjects();
    rtHeap().collect();
    CHECK(weakRefTargetSlot(wr).isUndefined());
    CHECK(rtWeakRefDeref(wr.get()).isUndefined());
}

TEST_CASE("a WeakRef that itself dies leaves the sweep table") {
    ShadowStackFrame frame;
    quiesce();

    const size_t before = rtWeakRefCellCount();
    {
        Rooted<Value> target{Value(bronze_create_object())};
        Rooted<Value> wr{rtMakeWeakRef(target)};
        CHECK(rtWeakRefCellCount() == before + 1);
    }
    rtClearKeptObjects();
    rtHeap().collect();
    // The table is the sweep's only handle on a WeakRef cell, so an entry that
    // outlived its object would have the sweep writing into recycled memory on
    // every later collection.
    CHECK(rtWeakRefCellCount() == before);
}

TEST_CASE("an unregistered symbol can be held weakly and a registered one cannot") {
    ShadowStackFrame frame;
    quiesce();

    Rooted<Value> fresh{rtMakeSymbol(Value::fromUndefined())};
    CHECK(rtCanBeHeldWeakly(fresh.get()));
    Rooted<Value> regKey{rtMakeString("weak_ref_registered")};
    Rooted<Value> registered{rtSymbolFor(regKey)};
    // 4.2.1: a registered symbol can always be re-minted from its string, so
    // nothing can ever observe it become unreachable.
    CHECK_FALSE(rtCanBeHeldWeakly(registered.get()));
    CHECK_FALSE(rtCanBeHeldWeakly(Value::fromDouble(1.0)));
    CHECK_FALSE(rtCanBeHeldWeakly(Value::fromNull()));

    Rooted<Value> wr{rtMakeWeakRef(fresh)};
    rtHeap().collect();
    CHECK(weakRefTargetSlot(wr).rawBits() == fresh.get().rawBits());
}

TEST_CASE("FinalizationRegistry: a dead target parks its held value, and a JOB calls back") {
    ShadowStackFrame frame;
    quiesce();

    Rooted<Value> cb{rtNativeFunction(recordCleanup, 1)};
    Rooted<Value> reg{rtMakeFinalizationRegistry(cb)};
    Rooted<Value> held{Value::fromDouble(42.0)};
    Rooted<Value> noToken{Value::fromUndefined()};
    const size_t cellsBefore = rtFinalizationCellCount();

    {
        Rooted<Value> target{Value(bronze_create_object())};
        rtFinalizationRegister(reg, target, held, noToken);
        CHECK(rtFinalizationCellCount() == cellsBefore + 1);
        // Registering does NOT add the target to the kept list, so a collection
        // with the target still rooted is the only reason the cell survives
        // this line — which is the half of weakness that must not over-fire.
        rtHeap().collect();
        CHECK(rtFinalizationCellCount() == cellsBefore + 1);
        CHECK_FALSE(rtFinalizationCleanupPending());
        CHECK(g_cleanupCalls == 0);
    }

    rtHeap().collect();
    CHECK(rtFinalizationCellCount() == cellsBefore);
    CHECK(rtFinalizationPendingCount() == 1);
    // The sweep runs INSIDE `collect()`, and it must not have called anything:
    // user code mid-collection would allocate on a heap in the middle of a
    // semispace flip.
    CHECK(g_cleanupCalls == 0);

    rtRunFinalizationCleanupJob();
    CHECK(g_cleanupCalls == 1);
    CHECK(g_lastHeldNumber == 42.0);
    CHECK_FALSE(rtFinalizationCleanupPending());

    // Idempotent: the cell is gone, so a second job has nothing to do.
    rtRunFinalizationCleanupJob();
    CHECK(g_cleanupCalls == 1);
}

TEST_CASE("a held value is retained STRONGLY until its callback has run") {
    ShadowStackFrame frame;
    quiesce();

    Rooted<Value> cb{rtNativeFunction(recordCleanup, 1)};
    Rooted<Value> reg{rtMakeFinalizationRegistry(cb)};
    {
        Rooted<Value> target{Value(bronze_create_object())};
        Rooted<Value> held{
            Value::fromString(StringHeader::createFromUTF8(rtHeap(), "held_marker"))};
        Rooted<Value> noToken{Value::fromUndefined()};
        rtFinalizationRegister(reg, target, held, noToken);
    }
    // Neither the target nor the held value is on any frame now. The target
    // must die; the held value must not, and must be forwarded twice — once out
    // of the cell table and once out of the pending queue.
    rtHeap().collect();
    rtHeap().collect();
    rtHeap().collect();

    rtRunFinalizationCleanupJob();
    CHECK(g_cleanupCalls == 1);
    CHECK(g_lastHeldText == "held_marker");
}

TEST_CASE("unregister removes a cell before it can ever fire") {
    ShadowStackFrame frame;
    quiesce();

    Rooted<Value> cb{rtNativeFunction(recordCleanup, 1)};
    Rooted<Value> reg{rtMakeFinalizationRegistry(cb)};
    Rooted<Value> token{Value(bronze_create_object())};
    Rooted<Value> held{Value::fromDouble(7.0)};
    const size_t cellsBefore = rtFinalizationCellCount();

    {
        Rooted<Value> target{Value(bronze_create_object())};
        rtFinalizationRegister(reg, target, held, token);
        // A token survives a collection like anything else that is rooted, and
        // `unregister` still finds the cell afterwards — which it can only do
        // if the sweep forwarded the token slot.
        rtHeap().collect();
        CHECK(rtFinalizationUnregister(reg, token));
        CHECK(rtFinalizationCellCount() == cellsBefore);
        // 26.2.3.2 answers false the second time: nothing left under that token.
        CHECK_FALSE(rtFinalizationUnregister(reg, token));
    }

    rtHeap().collect();
    CHECK_FALSE(rtFinalizationCleanupPending());
    rtRunFinalizationCleanupJob();
    CHECK(g_cleanupCalls == 0);
}

TEST_CASE("an unregister token is held WEAKLY: its death does not drop the registration") {
    ShadowStackFrame frame;
    quiesce();

    Rooted<Value> cb{rtNativeFunction(recordCleanup, 1)};
    Rooted<Value> reg{rtMakeFinalizationRegistry(cb)};
    Rooted<Value> target{Value(bronze_create_object())};
    Rooted<Value> held{Value::fromDouble(9.0)};
    const size_t cellsBefore = rtFinalizationCellCount();

    {
        Rooted<Value> token{Value(bronze_create_object())};
        rtFinalizationRegister(reg, target, held, token);
    }
    // The token is unreachable. That makes the cell UNUNREGISTERABLE and
    // nothing else: dropping it here would be a callback the program registered
    // and will never get.
    rtHeap().collect();
    CHECK(rtFinalizationCellCount() == cellsBefore + 1);
    CHECK_FALSE(rtFinalizationCleanupPending());

    // And the cell still fires on the target's death, with its token long gone.
    target.set(Value::fromUndefined());
    rtClearKeptObjects();
    rtHeap().collect();
    CHECK(rtFinalizationCellCount() == cellsBefore);
    rtRunFinalizationCleanupJob();
    CHECK(g_cleanupCalls == 1);
    CHECK(g_lastHeldNumber == 9.0);
}
