// The fast host function (embed/embed_fast_function.h): the NativeFn contract
// met with a stack copy of the arguments instead of a rooted block. The cases
// that matter are the GC ones — arguments read after the callback allocated
// and collected, from argument blocks the caller kept on the stack (up to
// kRootBlockInline) and on the C heap (past it), and the wide path past
// kFastInlineArgs — and the embed-gc-stress runs re-execute them with a
// collection at every allocation.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "embed/embed.h"
#include "embed/embed_fast_function.h"
#include "embed/embed_object_builder.h"
#include "runtime/heap.h"
#include "runtime/root_slots.h"
#include "runtime/rt_state.h"
#include "../stack_scrub.h"

using namespace bronze;

namespace {

// `count` fresh objects, each {i: index}: young when made, so a collection
// inside the callback would move every one of them that nothing pins.
std::vector<embed::Persistent> youngObjects(uint32_t count) {
    std::vector<embed::Persistent> out;
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        embed::Persistent o(embed::createObject());
        o.set(embed::setProperty(o.get(), "i", embed::fromDouble(i)));
        out.push_back(std::move(o));
    }
    return out;
}

// The callback: allocate enough to fill eden, collect (minor and full), then
// read every argument back. Returns the number whose `i` is still its index.
embed::Value churnThenCount(embed::Value, std::span<const embed::Value> args) {
    for (int k = 0; k < 2000; ++k) {
        embed::Value o = embed::createObject();
        embed::setProperty(o, "pad", embed::fromUtf8("some string to fill the young generation"));
    }
    runtime::rtHeap().collect_minor();
    runtime::rtHeap().collect();
    double good = 0;
    for (uint32_t i = 0; i < args.size(); ++i) {
        if (!args[i].isObject()) continue;
        if (embed::toDouble(embed::getProperty(args[i], "i")) == i) good += 1;
    }
    return embed::fromDouble(good);
}

double callWith(embed::Value fn, uint32_t argc) {
    std::vector<embed::Persistent> objs = youngObjects(argc);
    std::vector<embed::Value> args;
    for (auto& o : objs) args.push_back(o.get());
    embed::CallResult r = embed::call(fn, embed::undefined(), args);
    REQUIRE(!r.thrown);
    return r.value.asNumber();
}

}  // namespace

TEST_CASE("a fast host function sees its receiver and arguments") {
    int hits = 0;
    embed::Persistent fn{embed::makeFastFunction(
        [&hits](embed::Value self, std::span<const embed::Value> args) -> embed::Value {
            ++hits;
            double sum = embed::toDouble(self);
            for (const embed::Value& a : args) sum += embed::toDouble(a);
            return embed::fromDouble(sum);
        },
        2, "sum")};
    CHECK(embed::isFunction(fn.get()));
    CHECK(embed::toUtf8(embed::getProperty(fn.get(), "name")) == "sum");
    CHECK(embed::toDouble(embed::getProperty(fn.get(), "length")) == 2.0);

    std::vector<embed::Value> args{embed::fromDouble(2.0), embed::fromDouble(40.0)};
    embed::CallResult r = embed::call(fn.get(), embed::fromDouble(100.0), args);
    CHECK(!r.thrown);
    CHECK(r.value.asNumber() == 142.0);
    CHECK(hits == 1);

    // No arguments at all.
    embed::CallResult none = embed::call(fn.get(), embed::fromDouble(1.0), {});
    CHECK(!none.thrown);
    CHECK(none.value.asNumber() == 1.0);
}

TEST_CASE("a captureless fast host function and the raw function-pointer form") {
    embed::Persistent pure{embed::makeFastFunction(
        [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
            return embed::fromDouble(static_cast<double>(args.size()));
        },
        0)};
    std::vector<embed::Value> three{embed::null(), embed::null(), embed::null()};
    CHECK(embed::call(pure.get(), embed::undefined(), three).value.asNumber() == 3.0);

    static int payload = 5;
    embed::Persistent raw{embed::makeFastFunction(
        [](void* data, embed::Value, std::span<const embed::Value>) -> embed::Value {
            return embed::fromDouble(*static_cast<int*>(data));
        },
        &payload, nullptr, 0)};
    CHECK(embed::call(raw.get(), embed::undefined(), {}).value.asNumber() == 5.0);
}

TEST_CASE("fast host arguments stay current across the callback's allocations and collections") {
    embed::Persistent fn{embed::makeFastFunction(churnThenCount, 0)};
    // Within the caller's inline block, past it (the caller's block on the C
    // heap, only the stack copy pins), at the stack copy's limit, and past it
    // (the rooted wide path).
    const uint32_t counts[] = {1, 4, kRootBlockInline, kRootBlockInline + 3,
                               embed::kFastInlineArgs, embed::kFastInlineArgs + 5};
    for (uint32_t n : counts) {
        CAPTURE(n);
        CHECK(callWith(fn.get(), n) == static_cast<double>(n));
    }
}

TEST_CASE("a fast host function may return what it allocated") {
    embed::Persistent fn{embed::makeFastFunction(
        [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
            embed::ArrayBuilder arr;
            for (const embed::Value& a : args) {
                runtime::rtHeap().collect_minor();
                arr.push(a);
            }
            arr.push(embed::fromUtf8("tail"));
            return arr.get();
        },
        0)};
    std::vector<embed::Persistent> objs = youngObjects(3);
    std::vector<embed::Value> args{objs[0].get(), objs[1].get(), objs[2].get()};
    embed::CallResult r = embed::call(fn.get(), embed::undefined(), args);
    REQUIRE(!r.thrown);
    embed::Persistent out(r.value);
    runtime::rtHeap().collect();
    for (uint32_t i = 0; i < 3; ++i) {
        embed::Value e = embed::getElement(out.get(), i);
        REQUIRE(e.isObject());
        CHECK(embed::toDouble(embed::getProperty(e, "i")) == i);
    }
    CHECK(embed::toUtf8(embed::getElement(out.get(), 3)) == "tail");
}

TEST_CASE("a fast host function throws as a NativeFn does") {
    embed::Persistent jsThrow{embed::makeFastFunction(
        [](embed::Value, std::span<const embed::Value>) -> embed::Value {
            return embed::throwTypeError("Illegal invocation");
        },
        0)};
    embed::CallResult r = embed::call(jsThrow.get(), embed::undefined(), {});
    CHECK(r.thrown);
    REQUIRE(r.value.isObject());
    CHECK(embed::toUtf8(embed::getProperty(r.value, "message")) == "Illegal invocation");

    embed::Persistent cxxThrow{embed::makeFastFunction(
        [](embed::Value, std::span<const embed::Value>) -> embed::Value {
            throw std::invalid_argument("the library refused");
        },
        0)};
    embed::CallResult c = embed::call(cxxThrow.get(), embed::undefined(), {});
    CHECK(c.thrown);
    REQUIRE(c.value.isObject());
    CHECK(embed::toUtf8(embed::getProperty(c.value, "message")) == "the library refused");

    embed::Persistent oom{embed::makeFastFunction(
        [](embed::Value, std::span<const embed::Value>) -> embed::Value {
            throw std::bad_alloc();
        },
        0)};
    CHECK(embed::call(oom.get(), embed::undefined(), {}).thrown);

    // The boundary is clean afterwards.
    embed::Persistent ok{embed::makeFastFunction(
        [](embed::Value, std::span<const embed::Value>) -> embed::Value {
            return embed::fromDouble(7.0);
        },
        0)};
    embed::CallResult after = embed::call(ok.get(), embed::undefined(), {});
    CHECK(!after.thrown);
    CHECK(after.value.asNumber() == 7.0);
}

TEST_CASE("a fast host function's captured state is destroyed with the function") {
    struct Probe {
        int* destroyed;
        explicit Probe(int* d) : destroyed(d) {}
        Probe(Probe&& o) noexcept : destroyed(o.destroyed) { o.destroyed = nullptr; }
        ~Probe() {
            if (destroyed) ++*destroyed;
        }
    };
    static int destroyed = 0;
    destroyed = 0;
    test::inOwnFrame([] {
        Probe probe(&destroyed);
        embed::Persistent fn{embed::makeFastFunction(
            [p = std::move(probe)](embed::Value, std::span<const embed::Value>) -> embed::Value {
                return embed::fromBool(p.destroyed != nullptr);
            },
            0)};
        CHECK(embed::call(fn.get(), embed::undefined(), {}).value.isBool());
    });
    CHECK(destroyed == 0);
    test::scrubStack();
    runtime::rtHeap().collect();
    runtime::rtHeap().collect();
    CHECK(destroyed == 1);
}

TEST_CASE("ObjectBuilder::defFast installs a named fast method") {
    embed::ObjectBuilder b;
    b.defFast("twice", 1, [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
        return embed::fromDouble(2 * embed::toDouble(args.empty() ? embed::undefined() : args[0]));
    });
    embed::Persistent obj(b.build());
    embed::Value fn = embed::getProperty(obj.get(), "twice");
    REQUIRE(embed::isFunction(fn));
    CHECK(embed::toUtf8(embed::getProperty(fn, "name")) == "twice");
    std::vector<embed::Value> args{embed::fromDouble(21.0)};
    CHECK(embed::call(fn, obj.get(), args).value.asNumber() == 42.0);
}

// A microbenchmark, skipped unless asked for:
//
//   bronze_embed_tests -tc="*fast host call microbench*" --no-skip
//
// The same 3-argument body behind makeFunction and makeFastFunction, each
// called through embed::call (whose own cost is common to both, so the
// difference is the trampolines').
TEST_CASE("fast host call microbench" * doctest::skip()) {
    auto body = [](embed::Value self, std::span<const embed::Value> args) -> embed::Value {
        return embed::fromDouble(embed::toDouble(self) + static_cast<double>(args.size()));
    };
    embed::Persistent slow{embed::makeFunction(body, 3)};
    embed::Persistent fast{embed::makeFastFunction(body, 3)};
    std::vector<embed::Value> args{embed::fromDouble(1), embed::fromDouble(2), embed::fromDouble(3)};
    constexpr int kIters = 2'000'000;
    auto run = [&](embed::Value fn) {
        const auto t0 = std::chrono::steady_clock::now();
        double acc = 0;
        for (int i = 0; i < kIters; ++i) acc += embed::call(fn, embed::fromDouble(1), args).value.asNumber();
        const auto t1 = std::chrono::steady_clock::now();
        CHECK(acc == 4.0 * kIters);
        return std::chrono::duration<double, std::nano>(t1 - t0).count() / kIters;
    };
    for (int round = 0; round < 3; ++round) {
        const double s = run(slow.get());
        const double f = run(fast.get());
        std::printf("host call via embed::call: makeFunction %.2f ns, makeFastFunction %.2f ns\n", s, f);
    }
}
