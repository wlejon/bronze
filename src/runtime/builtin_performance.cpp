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

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

struct StoredPerformanceEntry {
    std::string name;
    std::string entryType;
    double startTime = 0.0;
    double duration = 0.0;
};

static std::vector<StoredPerformanceEntry>& performanceEntries() {
    static auto* list = new std::vector<StoredPerformanceEntry>();
    return *list;
}
static std::mutex g_performanceMutex;

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

double currentPerformanceTime() {
    const auto origin = timeOrigin();
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = now >= origin ? (now - origin) : std::chrono::nanoseconds::zero();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return static_cast<double>(ns) / 1e6;
}

Value createPerformanceEntryObject(const StoredPerformanceEntry& entry) {
    Rooted<Value> obj{Value::fromObject(
        ObjectHeader::create(rtHeap(), rtArena(), rtPlainObjectShape()))};
    obj.get().asObject<ObjectHeader>()->header.flags = HeapKind::Plain;

    Rooted<Value> nameKey{rtMakeString("name")};
    Rooted<Value> nameVal{rtMakeString(entry.name)};
    obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), nameKey, nameVal);

    Rooted<Value> typeKey{rtMakeString("entryType")};
    Rooted<Value> typeVal{rtMakeString(entry.entryType)};
    obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), typeKey, typeVal);

    Rooted<Value> startKey{rtMakeString("startTime")};
    Rooted<Value> startVal{Value::fromDouble(entry.startTime)};
    obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), startKey, startVal);

    Rooted<Value> durKey{rtMakeString("duration")};
    Rooted<Value> durVal{Value::fromDouble(entry.duration)};
    obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), durKey, durVal);

    return obj.get();
}

Value createEntryArray(const std::vector<StoredPerformanceEntry>& filtered) {
    uint32_t count = static_cast<uint32_t>(filtered.size());
    Rooted<Value> arr{Value::fromObject(ArrayHeader::create(rtHeap(), count))};
    for (uint32_t i = 0; i < count; ++i) {
        Rooted<Value> entryObj{createPerformanceEntryObject(filtered[i])};
        arr.get().asObject<ArrayHeader>()->setElem(rtHeap(), i, entryObj);
    }
    return arr.get();
}

// Milliseconds since the time origin, as a double with the sub-millisecond
// part intact. Counted in nanoseconds and divided rather than
// `duration_cast<milliseconds>`, which truncates to the integer this function
// exists to avoid.
uint64_t performanceNow(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return Value::fromDouble(currentPerformanceTime()).rawBits();
}

uint64_t performanceMark(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    std::string name = argc > 0 ? rtUtf8Chars(rtValueToString(Value(argv[0])).asString<StringHeader>()) : "undefined";
    double startTime = currentPerformanceTime();
    StoredPerformanceEntry entry{name, "mark", startTime, 0.0};
    {
        std::lock_guard<std::mutex> lock(g_performanceMutex);
        performanceEntries().push_back(entry);
    }
    return createPerformanceEntryObject(entry).rawBits();
}

uint64_t performanceMeasure(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    std::string name = argc > 0 ? rtUtf8Chars(rtValueToString(Value(argv[0])).asString<StringHeader>()) : "undefined";
    double startTime = 0.0;
    double endTime = currentPerformanceTime();

    {
        std::lock_guard<std::mutex> lock(g_performanceMutex);
        const auto& entries = performanceEntries();

        if (argc > 1 && !Value(argv[1]).isUndefined()) {
            if (Value(argv[1]).isNumber()) {
                startTime = Value(argv[1]).asNumber();
            } else {
                std::string startMarkName = rtUtf8Chars(rtValueToString(Value(argv[1])).asString<StringHeader>());
                for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
                    if (it->name == startMarkName && it->entryType == "mark") {
                        startTime = it->startTime;
                        break;
                    }
                }
            }
        }

        if (argc > 2 && !Value(argv[2]).isUndefined()) {
            if (Value(argv[2]).isNumber()) {
                endTime = Value(argv[2]).asNumber();
            } else {
                std::string endMarkName = rtUtf8Chars(rtValueToString(Value(argv[2])).asString<StringHeader>());
                for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
                    if (it->name == endMarkName && it->entryType == "mark") {
                        endTime = it->startTime;
                        break;
                    }
                }
            }
        }

        double duration = endTime >= startTime ? (endTime - startTime) : 0.0;
        StoredPerformanceEntry entry{name, "measure", startTime, duration};
        performanceEntries().push_back(entry);
        return createPerformanceEntryObject(entry).rawBits();
    }
}

uint64_t performanceClearMarks(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    std::lock_guard<std::mutex> lock(g_performanceMutex);
    auto& entries = performanceEntries();
    if (argc > 0 && !Value(argv[0]).isUndefined()) {
        std::string name = rtUtf8Chars(rtValueToString(Value(argv[0])).asString<StringHeader>());
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const StoredPerformanceEntry& e) {
            return e.entryType == "mark" && e.name == name;
        }), entries.end());
    } else {
        entries.erase(std::remove_if(entries.begin(), entries.end(), [](const StoredPerformanceEntry& e) {
            return e.entryType == "mark";
        }), entries.end());
    }
    return Value::fromUndefined().rawBits();
}

uint64_t performanceClearMeasures(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    std::lock_guard<std::mutex> lock(g_performanceMutex);
    auto& entries = performanceEntries();
    if (argc > 0 && !Value(argv[0]).isUndefined()) {
        std::string name = rtUtf8Chars(rtValueToString(Value(argv[0])).asString<StringHeader>());
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const StoredPerformanceEntry& e) {
            return e.entryType == "measure" && e.name == name;
        }), entries.end());
    } else {
        entries.erase(std::remove_if(entries.begin(), entries.end(), [](const StoredPerformanceEntry& e) {
            return e.entryType == "measure";
        }), entries.end());
    }
    return Value::fromUndefined().rawBits();
}

uint64_t performanceGetEntries(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    std::vector<StoredPerformanceEntry> copy;
    {
        std::lock_guard<std::mutex> lock(g_performanceMutex);
        copy = performanceEntries();
    }
    return createEntryArray(copy).rawBits();
}

uint64_t performanceGetEntriesByName(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    std::string name = argc > 0 ? rtUtf8Chars(rtValueToString(Value(argv[0])).asString<StringHeader>()) : "";
    std::string type = (argc > 1 && !Value(argv[1]).isUndefined()) ? rtUtf8Chars(rtValueToString(Value(argv[1])).asString<StringHeader>()) : "";
    std::vector<StoredPerformanceEntry> filtered;
    {
        std::lock_guard<std::mutex> lock(g_performanceMutex);
        for (const auto& e : performanceEntries()) {
            if (e.name == name && (type.empty() || e.entryType == type)) {
                filtered.push_back(e);
            }
        }
    }
    return createEntryArray(filtered).rawBits();
}

uint64_t performanceGetEntriesByType(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    std::string type = argc > 0 ? rtUtf8Chars(rtValueToString(Value(argv[0])).asString<StringHeader>()) : "";
    std::vector<StoredPerformanceEntry> filtered;
    {
        std::lock_guard<std::mutex> lock(g_performanceMutex);
        for (const auto& e : performanceEntries()) {
            if (e.entryType == type) {
                filtered.push_back(e);
            }
        }
    }
    return createEntryArray(filtered).rawBits();
}

using PerformanceFn = NativeMethod;

const PerformanceFn kPerformanceFunctions[] = {
    {"now", performanceNow, 0, 0},
    {"mark", performanceMark, 0, 1},
    {"measure", performanceMeasure, 0, 1},
    {"clearMarks", performanceClearMarks, 0, 0},
    {"clearMeasures", performanceClearMeasures, 0, 0},
    {"getEntries", performanceGetEntries, 0, 0},
    {"getEntriesByName", performanceGetEntriesByName, 0, 1},
    {"getEntriesByType", performanceGetEntriesByType, 0, 1},
};

double systemTimeOriginMs() {
    static const double origin_ms = []() {
        const auto now = std::chrono::system_clock::now();
        return std::chrono::duration<double, std::milli>(now.time_since_epoch()).count();
    }();
    return origin_ms;
}

// Real members of `performance` that bronze has NOT built. Reading one must not
// be `undefined` — a program that feature-tests `performance.mark` and finds it
// missing takes a branch no engine would take.
const char* const kPerformanceUnimplemented[] = {
    "toJSON", "eventCounter",
};

thread_local Value g_performanceObject = Value::fromUndefined();

}  // namespace

Value rtPerformanceNamespace() {
    if (g_performanceObject.isObject()) return g_performanceObject;

    // Its own root shape, not the one every `{}` literal shares — the same
    // reason `Math` mints one: a site reading `performance.now` and a site
    // reading `point.x` would otherwise walk one transition tree and miss each
    // other's caches forever.
    // Named prototype for the same reason `Math` names one: `performance` is
    // an ordinary object to everything that reflects over it, and a root shape
    // carrying `undefined` gave `Object.getPrototypeOf` nothing to answer with.
    Rooted<Value> obj{Value::fromObject(
        ObjectHeader::create(rtHeap(), rtArena(), rtNewRootShape(rtObjectPrototype())))};
    obj.get().asObject<ObjectHeader>()->header.flags = HeapKind::Plain;

    for (const PerformanceFn& fn : kPerformanceFunctions) {
        profileNameNative(reinterpret_cast<const void*>(fn.code), "performance", fn.name);
        Rooted<Value> key{rtMakeString(fn.name)};
        Rooted<Value> val{rtNativeFunction(fn.code, fn.arity, fn.name, fn.length)};
        obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
    }

    Rooted<Value> toKey{rtMakeString("timeOrigin")};
    Rooted<Value> toVal{Value::fromDouble(systemTimeOriginMs())};
    obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), toKey, toVal);

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
