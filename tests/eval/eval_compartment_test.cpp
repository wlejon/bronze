// Compartments (runtime/compartment.h): a realm's jobs on a fiber of its own,
// yielding at a loop backedge once their time slice is spent and resumed by
// the host, terminated with nothing the program can catch, each with its own
// microtask queue, and a collection with many of them suspended keeping what
// their frames hold. Each case runs on a thread of its own.
#include <doctest/doctest.h>

#include "embed/embed.h"
#include "eval/eval.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/rt_state.h"

#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace bronze;
using namespace bronze::eval;

namespace {

void onThread(const std::function<void()>& body) {
    std::thread worker([&] {
        ShadowStackFrame frame;
        body();
    });
    worker.join();
}

struct Eval {
    std::string source;
    ExecutionTier tier;
    std::string error;
};

void evalJob(void* arg) {
    auto* e = static_cast<Eval*>(arg);
    EvalOptions opts;
    opts.filename = "<compartment>";
    opts.tier = e->tier;
    embed::CallResult r = evalScript(e->source, opts);
    if (r.thrown) e->error = embed::toUtf8(r.value);
}

std::string globalString(embed::Realm* realm, const char* name) {
    embed::Value v = embed::getProperty(embed::getRealmGlobal(realm), name);
    if (embed::isString(v)) return embed::toUtf8(v);
    if (embed::isUndefined(v)) return "undefined";
    return "(not a string)";
}

const ExecutionTier kTiers[] = {ExecutionTier::Tier0_Interpreter, ExecutionTier::Tier1_Baseline,
                                ExecutionTier::Tier2_Optimized, ExecutionTier::Auto};

}  // namespace

TEST_CASE("compartment: a runaway loop yields at its slice in every tier, and terminating it ends it") {
    for (ExecutionTier tier : kTiers) {
        CAPTURE(static_cast<int>(tier));
        onThread([&] {
            embed::Realm* realm = embed::createRealm();
            embed::CompartmentOptions opts;
            opts.sliceMs = 5;
            embed::Compartment* c = embed::createCompartment(realm, opts);
            REQUIRE(c != nullptr);
            Eval e{"let i = 0; for (;;) { i++; globalThis.n = i; }", tier, {}};
            embed::CompartmentStatus s = embed::runInCompartment(c, &evalJob, &e);
            INFO(s.uncaught, " / ", s.terminationReason, " / ", e.error);
            REQUIRE(s.state == embed::CompartmentState::Yielded);
            // The host's own code runs on between turns.
            CHECK(embed::runningCompartment() == nullptr);
            s = embed::resumeCompartment(c);
            CHECK(s.state == embed::CompartmentState::Yielded);
            CHECK(globalString(realm, "n") == "(not a string)");  // a number: it ran
            embed::terminateCompartment(c, "not responding");
            CHECK(embed::compartmentState(c) == embed::CompartmentState::Terminated);
            // Dead: a later job does not run.
            Eval later{"globalThis.later = 'ran'", tier, {}};
            s = embed::runInCompartment(c, &evalJob, &later);
            CHECK(s.state == embed::CompartmentState::Terminated);
            CHECK(s.terminationReason == "not responding");
            CHECK(globalString(realm, "later") == "undefined");
            CHECK(embed::compartmentOverruns(c) >= 2);
            embed::destroyCompartment(c);
            embed::destroyRealm(realm);
        });
    }
}

TEST_CASE("compartment: a long job resumed slice by slice finishes with its result") {
    for (ExecutionTier tier : kTiers) {
        CAPTURE(static_cast<int>(tier));
        onThread([&] {
            embed::Realm* realm = embed::createRealm();
            embed::CompartmentOptions opts;
            opts.sliceMs = 2;
            embed::Compartment* c = embed::createCompartment(realm, opts);
            Eval e{"const t = performance.now(); let k = 0;"
                   "while (performance.now() - t < 150) k++;"
                   "globalThis.done = 'yes:' + (k > 0);",
                   tier, {}};
            embed::CompartmentStatus s = embed::runInCompartment(c, &evalJob, &e);
            int yields = 0;
            while (s.state == embed::CompartmentState::Yielded && yields < 10000) {
                ++yields;
                s = embed::resumeCompartment(c);
            }
            CHECK(s.state == embed::CompartmentState::Idle);
            CHECK(yields >= 2);
            CHECK(e.error.empty());
            CHECK(globalString(realm, "done") == "yes:true");
            embed::destroyCompartment(c);
            embed::destroyRealm(realm);
        });
    }
}

TEST_CASE("compartment: termination passes every catch and finally of the program") {
    for (ExecutionTier tier : kTiers) {
        CAPTURE(static_cast<int>(tier));
        onThread([&] {
            embed::Realm* realm = embed::createRealm();
            embed::CompartmentOptions opts;
            opts.sliceMs = 3;
            embed::Compartment* c = embed::createCompartment(realm, opts);
            Eval e{"function spin() { for (;;) {} }"
                   "try { try { spin(); } finally { globalThis.fin = 'ran'; } }"
                   "catch (err) { globalThis.caught = 'ran'; }",
                   tier, {}};
            embed::CompartmentStatus s = embed::runInCompartment(c, &evalJob, &e);
            CHECK(s.state == embed::CompartmentState::Yielded);
            embed::terminateCompartment(c, "stopped by the user");
            CHECK(embed::compartmentState(c) == embed::CompartmentState::Terminated);
            CHECK(globalString(realm, "fin") == "undefined");
            CHECK(globalString(realm, "caught") == "undefined");
            embed::destroyCompartment(c);
            embed::destroyRealm(realm);
        });
    }
}

TEST_CASE("compartment: a job's microtasks run in its own turn, on its own queue") {
    onThread([&] {
        embed::Realm* a = embed::createRealm();
        embed::Realm* b = embed::createRealm();
        embed::Compartment* ca = embed::createCompartment(a);
        embed::Compartment* cb = embed::createCompartment(b);
        Eval ea{"globalThis.order = ''; Promise.resolve().then(() => { globalThis.order += ',a-job'; });"
                "globalThis.order += 'a-sync';",
                ExecutionTier::Auto, {}};
        CHECK(embed::runInCompartment(ca, &evalJob, &ea).state == embed::CompartmentState::Idle);
        // A's reaction ran before its job returned, and none is left over.
        CHECK(globalString(a, "order") == "a-sync,a-job");
        CHECK_FALSE(embed::microtasksPending());
        Eval eb{"globalThis.order = ''; Promise.resolve().then(() => { globalThis.order += 'b-job'; });",
                ExecutionTier::Auto, {}};
        CHECK(embed::runInCompartment(cb, &evalJob, &eb).state == embed::CompartmentState::Idle);
        CHECK(globalString(b, "order") == "b-job");
        CHECK(globalString(a, "order") == "a-sync,a-job");
        embed::destroyCompartment(ca);
        embed::destroyCompartment(cb);
        embed::destroyRealm(a);
        embed::destroyRealm(b);
    });
}

TEST_CASE("compartment: an uncaught error is reported and the compartment lives on") {
    onThread([&] {
        embed::Realm* realm = embed::createRealm();
        embed::Compartment* c = embed::createCompartment(realm);
        Eval bad{"Promise.reject(new Error('later')); throw new Error('boom');", ExecutionTier::Auto, {}};
        embed::CompartmentStatus s = embed::runInCompartment(c, &evalJob, &bad);
        CHECK(s.state == embed::CompartmentState::Idle);
        CHECK(bad.error.find("boom") != std::string::npos);
        Eval good{"globalThis.after = 'ok'", ExecutionTier::Auto, {}};
        CHECK(embed::runInCompartment(c, &evalJob, &good).state == embed::CompartmentState::Idle);
        CHECK(globalString(realm, "after") == "ok");
        embed::destroyCompartment(c);
        embed::destroyRealm(realm);
    });
}

TEST_CASE("compartment: sixteen suspended compartments keep their frames' objects through collections") {
    onThread([&] {
        constexpr int kCount = 16;
        std::vector<embed::Realm*> realms;
        std::vector<embed::Compartment*> cs;
        std::vector<Eval> evals;
        evals.reserve(kCount);
        for (int k = 0; k < kCount; ++k) {
            realms.push_back(embed::createRealm());
            embed::CompartmentOptions opts;
            opts.sliceMs = 1;
            cs.push_back(embed::createCompartment(realms.back(), opts));
            evals.push_back(Eval{"const keep = { v: 'kept" + std::to_string(k) +
                                     "', big: new Array(1000).fill(" + std::to_string(k) +
                                     ") }; while (!globalThis.stop) { [1, 2, 3].map(x => x * 2); }"
                                     "globalThis.result = keep.v + ':' + keep.big[999];",
                                 ExecutionTier::Auto, {}});
        }
        for (int k = 0; k < kCount; ++k) {
            CHECK(embed::runInCompartment(cs[k], &evalJob, &evals[k]).state == embed::CompartmentState::Yielded);
        }
        for (int i = 0; i < 3; ++i) runtime::rtHeap().collect();
        for (int k = 0; k < kCount; ++k) {
            embed::setProperty(embed::getRealmGlobal(realms[k]), "stop", embed::Value::fromBool(true));
            embed::CompartmentStatus s = embed::resumeCompartment(cs[k]);
            while (s.state == embed::CompartmentState::Yielded) s = embed::resumeCompartment(cs[k]);
            CHECK(s.state == embed::CompartmentState::Idle);
            CHECK(globalString(realms[k], "result") == "kept" + std::to_string(k) + ":" + std::to_string(k));
        }
        for (int k = 0; k < kCount; ++k) {
            embed::destroyCompartment(cs[k]);
            embed::destroyRealm(realms[k]);
        }
    });
}

TEST_CASE("compartment: a typed-array loop suspended at its poll writes where its array lives after a collection") {
    // Each fill() is buffer-safe (it allocates nothing but, in the first, its
    // own array), so its element accesses go through a cursor: the array's
    // data pointer, derived once per block (il_property_lowering.cpp). The
    // array is young when the slice runs out, and the host's collection while
    // the fiber is suspended at the loop's poll moves it: the loop it resumes
    // into must write to the bytes the array has now, not the nursery copy it
    // was made in. A cursor taken once at the function's entry lost every
    // write after the first yield.
    const char* kCheck = "let ok = 0; for (let i = 0; i < 64; i++) if (a[i] === 400000) ok++;"
                         "globalThis.result = 'ok:' + ok;";
    const std::string kCases[] = {
        // A construction's cursor.
        std::string("function fill() {"
                    "  const a = new Float64Array(64);"
                    "  for (let r = 0; r < 400000; r++) {"
                    "    for (let i = 0; i < 64; i++) a[i] = a[i] + 1;"
                    "  }"
                    "  return a;"
                    "}"
                    "const a = fill();") + kCheck,
        // A parameter's.
        std::string("function fill(a) {"
                    "  for (let r = 0; r < 400000; r++) {"
                    "    for (let i = 0; i < 64; i++) a[i] = a[i] + 1;"
                    "  }"
                    "  return a;"
                    "}"
                    "const a = fill(new Float64Array(64));") + kCheck,
        // A captured binding's.
        std::string("function make() {"
                    "  const a = new Float64Array(64);"
                    "  function fill() {"
                    "    for (let r = 0; r < 400000; r++) {"
                    "      for (let i = 0; i < 64; i++) a[i] = a[i] + 1;"
                    "    }"
                    "  }"
                    "  fill();"
                    "  return a;"
                    "}"
                    "const a = make();") + kCheck,
    };
    for (const std::string& source : kCases) {
        for (ExecutionTier tier : kTiers) {
            onThread([&] {
                embed::Realm* realm = embed::createRealm();
                embed::CompartmentOptions opts;
                opts.sliceMs = 1;
                embed::Compartment* c = embed::createCompartment(realm, opts);
                Eval e{source, tier, {}};
                embed::CompartmentStatus s = embed::runInCompartment(c, &evalJob, &e);
                int yields = 0;
                while (s.state == embed::CompartmentState::Yielded && yields < 100000) {
                    ++yields;
                    runtime::rtHeap().collect();
                    s = embed::resumeCompartment(c);
                }
                INFO(source, " tier ", static_cast<int>(tier), ": ", s.uncaught, " / ", e.error);
                CHECK(s.state == embed::CompartmentState::Idle);
                CHECK(yields >= 1);
                CHECK(globalString(realm, "result") == "ok:64");
                embed::destroyCompartment(c);
                embed::destroyRealm(realm);
            });
        }
    }
}
