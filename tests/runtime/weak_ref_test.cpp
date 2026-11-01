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
//
// The collector keeps whatever a word of the stack names (stack pinning), so
// a target that is to die is made in a helper of its own (never inlined), the
// test frame reads its address only inside such helpers, and scrubStack()
// clears the dead frames before the collection that must find it dead
// (tests/stack_scrub.h).

#include <doctest/doctest.h>

#include <memory>
#include <string>

#include "../stack_scrub.h"

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
using bronze::test::scrubStack;

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

// A collection the stack's stale words cannot sway: the frames the helpers
// left are overwritten first.
void collectScrubbed() {
    scrubStack();
    rtHeap().collect();
}

void collectMinorScrubbed() {
    scrubStack();
    rtHeap().collect_minor();
}

// The weak tables are process-wide and every case in this binary shares them, so
// a case starts by settling whatever an earlier one left: clear the kept list,
// collect so every already-dead cell parks, drain those cleanup jobs, and only
// then zero the counters. Without this a case would be asserting about another
// case's garbage.
void quiesce() {
    rtClearKeptObjects();
    collectScrubbed();
    while (rtFinalizationCleanupPending()) rtRunFinalizationCleanupJob();
    g_cleanupCalls = 0;
    g_lastHeldNumber = 0.0;
    g_lastHeldText.clear();
}

BRONZE_TEST_NOINLINE bool weakTargetCleared(Rooted<Value>& wr) { return rtWeakRefTarget(wr.get()).isUndefined(); }

BRONZE_TEST_NOINLINE bool weakTargetIs(Rooted<Value>& wr, Rooted<Value>& target) {
    return rtWeakRefTarget(wr.get()).rawBits() == target.get().rawBits();
}

// A WeakRef (into `wr`) to a fresh object that nothing else holds once this
// returns; `young` says whether the target was young when made.
BRONZE_TEST_NOINLINE void weakRefToFreshObject(Rooted<Value>& wr, bool* young = nullptr) {
    Rooted<Value> target{Value(bronze_create_object())};
    wr.set(rtNewWeakRef(target));
    if (young) *young = rtHeap().is_movable(target.get().asObject());
}

}  // namespace

TEST_CASE("a WeakRef's target survives every collection while something else holds it") {
    ShadowStackFrame frame;
    quiesce();

    Rooted<Value> target{Value(bronze_create_object())};
    Rooted<Value> wr{rtNewWeakRef(target)};

    for (int i = 0; i < 3; ++i) rtHeap().collect();

    CHECK(weakTargetIs(wr, target));
    CHECK(rtWeakRefDeref(wr.get()).rawBits() == target.get().rawBits());
}

namespace {

// The target, rooted off the stack so that the collection moves it, and the
// WeakRef to it; the target's address as it was, masked, so that no word of
// the test's frame names it. Made with BRONZE_GC_STRESS's collections off:
// one run while the target's word is still on this frame pins it, and a
// later one can leave it old before the full collection that is to move it.
constexpr uint64_t kMaskOut = 0x5A5A5A5A5A5A5A5AULL;
BRONZE_TEST_NOINLINE uint64_t makeTargetAndWeakRef(Rooted<Value>& target, Rooted<Value>& wr) {
    const bool stress = rtHeap().gc_stress();
    rtHeap().set_gc_stress(false);
    target.set(Value(bronze_create_object()));
    wr.set(rtNewWeakRef(target));
    rtHeap().set_gc_stress(stress);
    REQUIRE(rtHeap().is_movable(target.get().asObject()));
    return rtWeakRefTarget(wr.get()).rawBits() ^ kMaskOut;
}

BRONZE_TEST_NOINLINE bool weakTargetIsNot(Rooted<Value>& wr, uint64_t maskedBits) {
    return rtWeakRefTarget(wr.get()).rawBits() != (maskedBits ^ kMaskOut);
}

}  // namespace

TEST_CASE("the weak slot is FORWARDED across a collection, not merely left readable") {
    ShadowStackFrame frame;
    quiesce();

    // A heap Rooted is a root no stack word pins: the full collection
    // promotes the young target, so it cannot stay where it was.
    auto target = std::make_unique<Rooted<Value>>(Value::fromUndefined());
    Rooted<Value> wr{Value::fromUndefined()};
    const uint64_t before = makeTargetAndWeakRef(*target, wr);

    collectScrubbed();

    CHECK(weakTargetIsNot(wr, before));
    CHECK(weakTargetIs(wr, *target));
}

namespace {

BRONZE_TEST_NOINLINE void derefFreshTarget(Rooted<Value>& wr) {
    Rooted<Value> target{Value(bronze_create_object())};
    wr.set(rtNewWeakRef(target));
    // 26.1.1.1 step 4 and 26.1.3.2 both add the target to [[KeptAlive]].
    CHECK(rtKeptObjectCount() >= 1);
    CHECK(rtWeakRefDeref(wr.get()).rawBits() == target.get().rawBits());
}

BRONZE_TEST_NOINLINE bool derefCleared(Rooted<Value>& wr) { return rtWeakRefDeref(wr.get()).isUndefined(); }

}  // namespace

TEST_CASE("KeepDuringJob: a deref'd target outlives its last reference until the list clears") {
    ShadowStackFrame frame;
    quiesce();

    Rooted<Value> wr{Value::fromUndefined()};
    derefFreshTarget(wr);

    // The target is out of every frame root now, and the ONLY thing left
    // referring to it is the kept-objects list. 9.13 says that is enough:
    // within one job a target that has been observed may not stop existing.
    collectScrubbed();
    CHECK_FALSE(weakTargetCleared(wr));

    // ClearKeptObjects is the microtask checkpoint's, and this is what it buys:
    // the same collection that changed nothing a moment ago now clears the slot.
    rtClearKeptObjects();
    collectScrubbed();
    CHECK(weakTargetCleared(wr));
    CHECK(derefCleared(wr));
}

namespace {

BRONZE_TEST_NOINLINE void makeObjectInto(Rooted<Value>& slot) { slot.set(Value(bronze_create_object())); }

BRONZE_TEST_NOINLINE void makeWeakRefInto(Rooted<Value>& wr, Rooted<Value>& target) {
    wr.set(rtNewWeakRef(target));
}

BRONZE_TEST_NOINLINE bool isYoung(Rooted<Value>& v) { return rtHeap().is_movable(v.get().asObject()); }

// A WeakRef (into `wrOld`, a root off the stack) to a fresh object, both of
// which a full collection has promoted, and nothing else holds the target
// once this returns. A young object a stack word names is pinned and stays
// young, so both are held through heap Rooteds while it promotes them.
BRONZE_TEST_NOINLINE void oldWeakRefToOldTarget(Rooted<Value>& wrOld) {
    auto target = std::make_unique<Rooted<Value>>(Value::fromUndefined());
    makeObjectInto(*target);
    makeWeakRefInto(wrOld, *target);
    collectScrubbed();
    REQUIRE_FALSE(isYoung(*target));
    REQUIRE_FALSE(isYoung(wrOld));
}

}  // namespace

TEST_CASE("a young target is cleared by a minor collection, an old one only by a full one") {
    ShadowStackFrame frame;
    quiesce();

    // Young WeakRef, young target: the minor collection decides both.
    Rooted<Value> wr{Value::fromUndefined()};
    bool targetYoung = false;
    weakRefToFreshObject(wr, &targetYoung);
    rtClearKeptObjects();
    collectMinorScrubbed();
    // Under BRONZE_GC_STRESS every allocation above collected, and a target
    // that survived enough of them is already old, which only a full
    // collection decides. Run alone (or first) the case always met one.
    if (!targetYoung) collectScrubbed();
    CHECK(weakTargetCleared(wr));

    // An old target outlives a minor collection however unreachable it is,
    // and a full one clears it — through an old WeakRef, which no minor
    // collection scans.
    auto wrOld = std::make_unique<Rooted<Value>>(Value::fromUndefined());
    oldWeakRefToOldTarget(*wrOld);
    rtClearKeptObjects();
    collectMinorScrubbed();
    CHECK_FALSE(weakTargetCleared(*wrOld));
    collectScrubbed();
    CHECK(weakTargetCleared(*wrOld));
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

    Rooted<Value> wr{rtNewWeakRef(fresh)};
    rtHeap().collect();
    CHECK(weakTargetIs(wr, fresh));
}

namespace {

BRONZE_TEST_NOINLINE void registerFreshTarget(Rooted<Value>& reg, Rooted<Value>& held, Rooted<Value>& token,
                                              size_t cellsBefore) {
    Rooted<Value> target{Value(bronze_create_object())};
    rtFinalizationRegister(reg, target, held, token);
    CHECK(rtFinalizationCellCount() == cellsBefore + 1);
    // Registering does NOT add the target to the kept list, so a collection
    // with the target still rooted is the only reason the cell survives
    // this line — which is the half of weakness that must not over-fire.
    rtHeap().collect();
    CHECK(rtFinalizationCellCount() == cellsBefore + 1);
    CHECK_FALSE(rtFinalizationCleanupPending());
    CHECK(g_cleanupCalls == 0);
}

}  // namespace

TEST_CASE("FinalizationRegistry: a dead target parks its held value, and a JOB calls back") {
    ShadowStackFrame frame;
    quiesce();

    Rooted<Value> cb{rtNativeFunction(recordCleanup, 1)};
    Rooted<Value> reg{rtNewFinalizationRegistry(cb)};
    Rooted<Value> held{Value::fromDouble(42.0)};
    Rooted<Value> noToken{Value::fromUndefined()};
    const size_t cellsBefore = rtFinalizationCellCount();

    registerFreshTarget(reg, held, noToken, cellsBefore);

    collectScrubbed();
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

namespace {

BRONZE_TEST_NOINLINE void registerFreshTargetHoldingString(Rooted<Value>& reg) {
    Rooted<Value> target{Value(bronze_create_object())};
    Rooted<Value> held{Value::fromString(StringHeader::createFromUTF8(rtHeap(), "held_marker"))};
    Rooted<Value> noToken{Value::fromUndefined()};
    rtFinalizationRegister(reg, target, held, noToken);
}

}  // namespace

TEST_CASE("a held value is retained STRONGLY until its callback has run") {
    ShadowStackFrame frame;
    quiesce();

    Rooted<Value> cb{rtNativeFunction(recordCleanup, 1)};
    Rooted<Value> reg{rtNewFinalizationRegistry(cb)};
    registerFreshTargetHoldingString(reg);
    // Neither the target nor the held value is on any frame now. The target
    // must die; the held value must not, and must be forwarded twice — once out
    // of the cell table and once out of the pending queue.
    collectScrubbed();
    collectScrubbed();
    collectScrubbed();

    rtRunFinalizationCleanupJob();
    CHECK(g_cleanupCalls == 1);
    CHECK(g_lastHeldText == "held_marker");
}

namespace {

BRONZE_TEST_NOINLINE void registerAndUnregister(Rooted<Value>& reg, Rooted<Value>& held, Rooted<Value>& token,
                                                size_t cellsBefore) {
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

}  // namespace

TEST_CASE("unregister removes a cell before it can ever fire") {
    ShadowStackFrame frame;
    quiesce();

    Rooted<Value> cb{rtNativeFunction(recordCleanup, 1)};
    Rooted<Value> reg{rtNewFinalizationRegistry(cb)};
    Rooted<Value> token{Value(bronze_create_object())};
    Rooted<Value> held{Value::fromDouble(7.0)};
    const size_t cellsBefore = rtFinalizationCellCount();

    registerAndUnregister(reg, held, token, cellsBefore);

    collectScrubbed();
    CHECK_FALSE(rtFinalizationCleanupPending());
    rtRunFinalizationCleanupJob();
    CHECK(g_cleanupCalls == 0);
}

namespace {

BRONZE_TEST_NOINLINE void registerWithFreshToken(Rooted<Value>& reg, Rooted<Value>& target, Rooted<Value>& held) {
    Rooted<Value> token{Value(bronze_create_object())};
    rtFinalizationRegister(reg, target, held, token);
}

}  // namespace

TEST_CASE("an unregister token is held WEAKLY: its death does not drop the registration") {
    ShadowStackFrame frame;
    quiesce();

    Rooted<Value> cb{rtNativeFunction(recordCleanup, 1)};
    Rooted<Value> reg{rtNewFinalizationRegistry(cb)};
    Rooted<Value> target{Value::fromUndefined()};
    makeObjectInto(target);
    Rooted<Value> held{Value::fromDouble(9.0)};
    const size_t cellsBefore = rtFinalizationCellCount();

    registerWithFreshToken(reg, target, held);
    // The token is unreachable. That makes the cell UNUNREGISTERABLE and
    // nothing else: dropping it here would be a callback the program registered
    // and will never get.
    collectScrubbed();
    CHECK(rtFinalizationCellCount() == cellsBefore + 1);
    CHECK_FALSE(rtFinalizationCleanupPending());

    // And the cell still fires on the target's death, with its token long gone.
    target.set(Value::fromUndefined());
    rtClearKeptObjects();
    collectScrubbed();
    CHECK(rtFinalizationCellCount() == cellsBefore);
    rtRunFinalizationCleanupJob();
    CHECK(g_cleanupCalls == 1);
    CHECK(g_lastHeldNumber == 9.0);
}
