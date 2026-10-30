// A marking cycle driven through the host API (embed_gc.h) with the program
// running between its steps: what a host that collects between frames does.
// JS between the slices allocates, links young and old objects into old
// ones, re-links and unlinks old ones, writes globals, makes and drops eval /
// new Function modules, and holds things weakly; the cycle must keep every
// object the program can still reach and reclaim what it could not reach
// when the cycle started (what died during it may float to the next cycle).
//
// Run under BRASS_GC_VERIFY=1 too: the finish then marks the heap afresh and
// stops on anything live the cycle left unmarked, and every root table's
// skipped cells are checked at each minor collection.

#include <doctest/doctest.h>

#include <cstdlib>
#include <string>

#include "embed/embed.h"
#include "embed/embed_gc.h"
#include "eval/eval.h"
#include "runtime/rt_state.h"

using namespace bronze;
using namespace bronze::eval;

namespace {

double num(const char* src, const EvalOptions& opts = {}) {
    embed::CallResult r = evalScript(src, opts);
    REQUIRE_MESSAGE(!r.thrown, src);
    REQUIRE_MESSAGE(r.value.isNumber(), src);
    return r.value.asNumber();
}

void run(const std::string& src, const EvalOptions& opts = {}) {
    embed::CallResult r = evalScript(src, opts);
    REQUIRE_MESSAGE(!r.thrown, src);
}

// Under BRASS_GC_STRESS the heap starts and finishes cycles of its own at
// allocations, as a host's cycle may be finished by the heap's trigger: the
// steps below then do what a host does, look before each step, and the
// checks that count collections are left out.
bool stressed() {
    const char* v = std::getenv("BRASS_GC_STRESS");
    return v != nullptr && *v != '\0' && std::string(v) != "0";
}

// Opens a cycle; under stress one may be open already (or none can be had).
void startCycle() {
    if (embed::startGcCycle()) return;
    REQUIRE(stressed());
}

// A mark slice if a cycle is open: true once there is nothing left to mark.
bool markSlice(uint64_t budgetNs) {
    if (!embed::heapPressure().marking) return true;
    return embed::gcMarkSlice(budgetNs);
}

void finishCycle() {
    if (embed::heapPressure().marking) embed::finishGcCycle();
}

// A whole cycle with nothing running between its steps.
void wholeCycle() {
    startCycle();
    while (!markSlice(1'000'000)) {
    }
    finishCycle();
}

// What the cycle's sweep leaves, swept with the program allocating and
// re-linking between the slices (T.light: no new modules, so a count of them
// taken after the finish still holds).
void sweepOut(int& steps, const EvalOptions& opts) {
    while (!embed::gcSweepSlice(2000)) {
        run("T.light(" + std::to_string(steps++) + ")", opts);
    }
}

// The program's state after the setup: a graph of old nodes, each naming
// another, a sum the graph must still give, and things held only weakly.
const char* kSetup = R"JS(
globalThis.T = {
    N: 3000,
    nodes: [],
    fns: [],
    cleaned: [],
    globalsWritten: 0,
};
for (let i = 0; i < T.N; i++) T.nodes.push({ id: i, next: null, young: null, data: [i, 'n' + i] });
for (let i = 0; i < T.N; i++) T.nodes[i].next = T.nodes[(i * 7 + 1) % T.N];
T.fr = new FinalizationRegistry((held) => { T.cleaned.push(held); });
T.goneBefore = { tag: 'before', payload: new Array(64).fill(1) };
T.goneDuring = { tag: 'during', payload: new Array(64).fill(2) };
T.wrBefore = new WeakRef(T.goneBefore);
T.wrDuring = new WeakRef(T.goneDuring);
T.wrLive = new WeakRef(T.nodes[5]);
T.fr.register(T.goneBefore, 'before');
T.fr.register(T.goneDuring, 'during');
T.wm = new WeakMap();
T.wm.set(T.nodes[9], { side: 9 });
T.wmGoneKey = { tag: 'wm-key' };
T.wm.set(T.wmGoneKey, { side: 'gone' });
T.wrWmValue = new WeakRef(T.wm.get(T.wmGoneKey));
// One step of the program between two of the collector's: young objects
// stored into old ones, old ones re-linked to old ones and unlinked, a
// global written, a closure kept from new Function and one from eval, and
// a transient eval whose module nothing keeps.
T.step = function (s) {
    const n = T.N;
    for (let k = 0; k < 40; k++) {
        const a = T.nodes[(s * 131 + k * 17) % n];
        a.young = { s, k, list: [s, k, { deep: s + k }] };
        const b = T.nodes[(s * 37 + k * 11) % n];
        const c = T.nodes[(s * 53 + k * 29 + 3) % n];
        b.next = c;
        if ((k & 7) === 0) T.nodes[(s * 71 + k) % n].young = null;
    }
    globalThis['g' + (s % 16)] = { s, ref: T.nodes[s % n] };
    T.globalsWritten++;
    if (s % 5 === 0) T.fns.push(new Function('x', 'return x + ' + s + ';'));
    if (s % 7 === 0) T.fns.push(eval('(function (x) { return x * 2 + ' + s + '; })'));
    eval('(function () { return ' + s + '; })()');
    return s;
};
T.light = function (s) {
    for (let k = 0; k < 40; k++) {
        T.nodes[(s * 131 + k * 17) % T.N].young = { s, k, list: [s, k, { deep: s + k }] };
        T.nodes[(s * 37 + k * 11) % T.N].next = T.nodes[(s * 53 + k * 29 + 3) % T.N];
    }
    return s;
};
// Every node there, each naming a node, every young object intact.
T.check = function () {
    let sum = 0;
    for (let i = 0; i < T.N; i++) {
        const o = T.nodes[i];
        if (o.id !== i || o.data[0] !== i || o.data[1] !== 'n' + i) return -1;
        if (!o.next || typeof o.next.id !== 'number') return -2;
        if (o.young !== null && o.young.list[2].deep !== o.young.s + o.young.k) return -3;
        sum += o.id;
    }
    for (let j = 0; j < T.fns.length; j++) if (typeof T.fns[j](1) !== 'number') return -4;
    for (let g = 0; g < 16; g++) {
        const v = globalThis['g' + g];
        if (v !== undefined && v.ref.id !== v.s % T.N) return -5;
    }
    if (T.wm.get(T.nodes[9]).side !== 9) return -6;
    return sum;
};
undefined;
)JS";

}  // namespace

TEST_CASE("a cycle driven through embed_gc with the program running between its steps keeps what is live") {
    installDefaultDynamicHooks();
    for (ExecutionTier tier : {ExecutionTier::Auto, ExecutionTier::Tier2_Optimized}) {
        CAPTURE(static_cast<int>(tier));
        EvalOptions opts;
        opts.tier = tier;
        run(kSetup, opts);
        embed::collectGarbage();
        // The setup's own module, if it is one, and whatever earlier tests left.
        const size_t baselineModules = runtime::rtDynamicModuleCount();
        const double expected = num("T.check()", opts);
        REQUIRE(expected == 3000.0 * 2999.0 / 2.0);
        // Everything the setup made is old, and the two "gone" objects are
        // held only weakly from here: unreachable when the cycle starts.
        run("delete T.goneBefore; delete T.wmGoneKey; undefined;", opts);
        embed::collectGarbage();
        embed::clearKeptObjects();
        while (embed::heapPressure().sweeping) embed::gcSweepSlice(1'000'000);

        if (!stressed()) {
            REQUIRE_FALSE(embed::heapPressure().marking);
            REQUIRE(embed::startGcCycle());
            CHECK(embed::heapPressure().marking);
            CHECK_FALSE(embed::startGcCycle());
        } else {
            startCycle();
        }

        // Tiny slices, the program between each: dozens of steps inside the
        // cycle, a minor collection among them now and then.
        int steps = 0;
        run("delete T.goneDuring; undefined;", opts);
        bool done = false;
        for (int i = 0; i < 400 && !done; ++i) {
            done = markSlice(2000);
            run("T.step(" + std::to_string(steps++) + ")", opts);
            if (i % 9 == 4) embed::collectYoungGarbage();
            if (i < 30) done = false;  // at least thirty steps inside the cycle
        }
        // Whatever the last steps shaded is drained by the finish.
        const uint64_t fullBefore = embed::getRuntimeTelemetry().gcFullCollections;
        finishCycle();
        if (!stressed()) {
            CHECK_FALSE(embed::heapPressure().marking);
            CHECK(embed::getRuntimeTelemetry().gcFullCollections == fullBefore + 1);
        }
        // The eval / new Function modules the kept closures name are kept, the
        // transient evals' are gone.
        const size_t kept = static_cast<size_t>(num("T.fns.length", opts));
        CHECK(kept > 0);
        CHECK(runtime::rtDynamicModuleCount() <= baselineModules + kept);
        sweepOut(steps, opts);

        CHECK(num("T.check()", opts) == expected);
        embed::clearKeptObjects();
        embed::drainMicrotasks();
        // Unreachable at the start: reclaimed, its registration cleaned up.
        CHECK(num("T.wrBefore.deref() === undefined ? 1 : 0", opts) == 1.0);
        CHECK(num("T.cleaned.indexOf('before') >= 0 ? 1 : 0", opts) == 1.0);
        CHECK(num("T.wrWmValue.deref() === undefined ? 1 : 0", opts) == 1.0);
        // Live throughout: kept.
        CHECK(num("T.wrLive.deref() === T.nodes[5] ? 1 : 0", opts) == 1.0);

        // Unreachable during the cycle: it may have floated through it, and
        // the next cycle (no program between its steps) reclaims it.
        wholeCycle();
        sweepOut(steps, opts);
        embed::clearKeptObjects();
        embed::drainMicrotasks();
        CHECK(num("T.wrDuring.deref() === undefined ? 1 : 0", opts) == 1.0);
        CHECK(num("T.cleaned.indexOf('during') >= 0 ? 1 : 0", opts) == 1.0);
        CHECK(num("T.check()", opts) == expected);

        // Dropped closures: their modules go at a cycle's finish. Not
        // necessarily the next one's: a root cache (a call site's IC, a
        // global's cache cell) may still name a closure when a cycle starts,
        // and what its start visits is kept through that cycle even if the
        // program overwrites the cell before the finish.
        run("T.fns.length = 0; undefined;", opts);
        embed::clearKeptObjects();
        startCycle();
        run("T.light(" + std::to_string(steps++) + ")", opts);
        while (!markSlice(1'000'000)) {
        }
        finishCycle();
        sweepOut(steps, opts);
        wholeCycle();
        sweepOut(steps, opts);
        CHECK(runtime::rtDynamicModuleCount() <= baselineModules);
        CHECK(num("T.check()", opts) == expected);
        run("delete globalThis.T; for (let g = 0; g < 16; g++) delete globalThis['g' + g]; undefined;", opts);
    }
}

TEST_CASE("collectGarbage during an open cycle finishes it, and the heap's state says so") {
    run("globalThis.U = []; for (let i = 0; i < 1000; i++) U.push({ i }); undefined;");
    embed::collectGarbage();
    startCycle();
    markSlice(1000);
    run("U.push({ late: true }); undefined;");
    const uint64_t fullBefore = embed::getRuntimeTelemetry().gcFullCollections;
    embed::collectGarbage();
    // The cycle's finish and the whole collection after it.
    if (!stressed()) {
        CHECK_FALSE(embed::heapPressure().marking);
        CHECK(embed::getRuntimeTelemetry().gcFullCollections == fullBefore + 2);
    }
    CHECK(num("U.length === 1001 && U[1000].late && U[999].i === 999 ? 1 : 0") == 1.0);
    run("delete globalThis.U; undefined;");
}
