// The `performance` namespace, for `performance.now()` (High Resolution Time,
// W3C hr-time-3). One object, built once, arranged exactly like `Math`.
//
// bronze provides it for the same reason a browser does: it is the clock JS
// reaches for when it wants to measure itself. `Date.now()` is integer
// milliseconds by specification (ECMA-262 21.4.3.1), which cannot resolve a
// region shorter than a millisecond and cannot be trusted to move forward at
// all — it reads a wall clock an NTP step can drag backwards mid-measurement.
// `performance.now()` is neither: it is a monotonic count of milliseconds as a
// double, taken from a clock nothing outside the process can adjust.
//
// three.js asks `typeof performance === 'undefined' ? Date : performance` and
// pixi calls `performance.now()` outright, so providing the name is also what
// stops two of bronze's target libraries from silently taking their fallback
// path.

#include <chrono>
#include <cstdint>
#include <iterator>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/fn.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_state.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// The time origin (hr-time-3 §4), fixed at the first read rather than at
// process start: it only has to be a constant, and taking it lazily keeps this
// translation unit off the startup path of a program that never asks the time.
// `steady_clock` is the monotonic one — the whole point of this clock over
// `Date.now()` is that no wall-clock adjustment can move it.
std::chrono::steady_clock::time_point timeOrigin() {
    static const std::chrono::steady_clock::time_point origin =
        std::chrono::steady_clock::now();
    return origin;
}

// Milliseconds since the time origin, as a double with the sub-millisecond
// part intact. Counted in nanoseconds and divided rather than
// `duration_cast<milliseconds>`, which truncates to the integer this function
// exists to avoid.
uint64_t performanceNow(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    const auto elapsed = std::chrono::steady_clock::now() - timeOrigin();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return Value::fromDouble(static_cast<double>(ns) / 1e6).rawBits();
}

struct PerformanceFn {
    const char* name;
    bronze_fn_code code;
    uint32_t arity;
};

const PerformanceFn kPerformanceFunctions[] = {
    {"now", performanceNow, 0},
};

// Real members of `performance` that bronze has NOT built. Reading one must not
// be `undefined` — a program that feature-tests `performance.mark` and finds it
// missing takes a branch no engine would take. Same rule as `Math`'s table:
// membership here is "does the platform have this?", never "have we got round
// to it?". The whole User Timing and Resource Timing surface sits behind an
// observer and a buffer bronze has no host for.
const char* const kPerformanceUnimplemented[] = {
    "mark",       "measure",           "clearMarks",     "clearMeasures",
    "getEntries", "getEntriesByName",  "getEntriesByType",
    "timeOrigin", "toJSON",            "eventCounter",
};

thread_local Value g_performanceObject = Value::fromUndefined();

}  // namespace

Value rtPerformanceNamespace() {
    if (g_performanceObject.isObject()) return g_performanceObject;

    // Its own root shape, not the one every `{}` literal shares — the same
    // reason `Math` mints one: a site reading `performance.now` and a site
    // reading `point.x` would otherwise walk one transition tree and miss each
    // other's caches forever.
    Rooted<Value> obj{Value::fromObject(
        ObjectHeader::create(rtHeap(), rtArena(), rtNewRootShape(Value::fromUndefined())))};
    obj.get().asObject<ObjectHeader>()->header.flags = HeapKind::Plain;

    for (const PerformanceFn& fn : kPerformanceFunctions) {
        profileNameNative(reinterpret_cast<const void*>(fn.code), "performance", fn.name);
        Rooted<Value> key{rtMakeString(fn.name)};
        Rooted<Value> val{rtNativeFunction(fn.code, fn.arity)};
        obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
    }

    // hr-time-3 gives `Performance` a @@toStringTag of "Performance", which is
    // what makes `Object.prototype.toString.call(performance)` read
    // "[object Performance]".
    rtDefineToStringTag(obj, "Performance");

    g_performanceObject = obj.get();
    rtHeap().add_permanent_root(&g_performanceObject);
    return g_performanceObject;
}

bool rtPerformanceCheckMissingMember(Value obj, const std::string& key) {
    if (!g_performanceObject.isObject() ||
        obj.rawBits() != g_performanceObject.rawBits()) {
        return false;
    }
    rtCheckUnimplementedMember("performance", kPerformanceUnimplemented,
                               std::size(kPerformanceUnimplemented), key);
    return true;
}

}  // namespace bronze::runtime
