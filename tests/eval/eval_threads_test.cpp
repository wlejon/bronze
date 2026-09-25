// One compiled program run on several threads (EvalOptions::
// shareAcrossThreads): each thread's run starts from fresh module data, and
// all of them share the program's code, including what tiers up while they
// run.
#include <doctest/doctest.h>

#include "codegen-brass/brass_tiered_engine.h"
#include "embed/embed.h"
#include "eval/eval.h"
#include "runtime/gc.h"

#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace bronze;
using namespace bronze::eval;

namespace {

EvalOptions sharedOptions(const char* name) {
    EvalOptions opts;
    opts.filename = name;
    opts.shareAcrossThreads = true;
    opts.tier = ExecutionTier::Auto;
    // Named, not this thread's registrations: a thread's host globals are
    // part of what a program compiles against, and the test's threads
    // register different ones.
    opts.hostGlobals = {"console"};
    return opts;
}

// Module state a second run on one instance would see: `counter` starts at
// 0 only in fresh module data.
constexpr const char* kCounterSource =
    "var counter = 0;\n"
    "function bump() { return ++counter; }\n"
    "bump();\n"
    "bump();\n";

// Long enough to tier up and to enter its loop's OSR code.
constexpr const char* kHotSource =
    "let s = 0;\n"
    "for (let i = 0; i < 3000000; i++) { s = (s + i * 7) % 1000003; }\n"
    "s;\n";

double expectedHot() {
    long long s = 0;
    for (long long i = 0; i < 3000000; ++i) s = (s + i * 7) % 1000003;
    return static_cast<double>(s);
}

}  // namespace

TEST_CASE("a shared program compiles once and each thread runs it from fresh module data") {
    const EvalOptions opts = sharedOptions("<share-counter>");
    auto first = compileScript(kCounterSource, opts);
    REQUIRE(first->success);
    auto again = compileScript(kCounterSource, opts);
    REQUIRE(again->success);
    // Not run anywhere yet: the same program, under the same result name.
    CHECK(first->program == again->program);
    CHECK(first->resName == again->resName);

    const BrassTieredProgram* ranHere = first->program.get();
    embed::CallResult r = runCompiledScript(std::move(first), opts);
    CHECK(!r.thrown);
    CHECK(r.value.asNumber() == 2.0);

    // This thread ran it: a second evaluation here compiles again.
    auto fresh = compileScript(kCounterSource, opts);
    REQUIRE(fresh->success);
    CHECK(fresh->program.get() != ranHere);
    const BrassTieredProgram* cached = fresh->program.get();
    CHECK(runCompiledScript(std::move(fresh), opts).value.asNumber() == 2.0);

    // Another thread takes the program this one compiled last, and its run
    // starts from counter = 0 as well.
    const BrassTieredProgram* onThread = nullptr;
    double threadResult = 0;
    bool threadThrew = true;
    std::thread worker([&] {
        ShadowStackFrame frame;
        auto script = compileScript(kCounterSource, opts);
        onThread = script->program.get();
        embed::CallResult tr = runCompiledScript(std::move(script), opts);
        threadThrew = tr.thrown;
        if (!tr.thrown) threadResult = tr.value.asNumber();
    });
    worker.join();
    CHECK(onThread == cached);
    CHECK(!threadThrew);
    CHECK(threadResult == 2.0);
}

TEST_CASE("threads running one shared program at once each get its result") {
    const EvalOptions opts = sharedOptions("<share-hot>");
    const double want = expectedHot();
    constexpr int kThreads = 3;
    std::vector<double> results(kThreads, -1.0);
    std::vector<const BrassTieredProgram*> programs(kThreads, nullptr);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            ShadowStackFrame frame;
            auto script = compileScript(kHotSource, opts);
            programs[t] = script->program.get();
            embed::CallResult r = runCompiledScript(std::move(script), opts);
            if (!r.thrown) results[t] = r.value.asNumber();
        });
    }
    for (auto& th : threads) th.join();
    for (int t = 0; t < kThreads; ++t) CHECK(results[t] == want);
    // All of them compiled under one lock; the first compiled and the rest
    // took its program unless they ran it themselves first (none did).
    for (int t = 1; t < kThreads; ++t) CHECK(programs[t] == programs[0]);
}

TEST_CASE("programs compiled without sharing are never shared") {
    EvalOptions opts;
    opts.filename = "<no-share>";
    auto a = compileScript(kCounterSource, opts);
    auto b = compileScript(kCounterSource, opts);
    REQUIRE(a->success);
    REQUIRE(b->success);
    CHECK(a->program != b->program);
}
