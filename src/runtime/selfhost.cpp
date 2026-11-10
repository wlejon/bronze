// The self-hosted builtins' runtime half (selfhost.h, src/selfhost/README.md):
// the registry the library's entry fills, the lazy run of that entry, and the
// helpers its intrinsics call (lower_selfhost.cpp, kHelpers).

#include "runtime/selfhost.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <limits>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/builtin_array_internal.h"
#include "runtime/builtin_object.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/heap.h"
#include "runtime/host_globals.h"
#include "runtime/module_instance.h"
#include "runtime/native_base.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/value.h"

// The library's entry (src/selfhost, built by bronze_stage0).
extern "C" void bronze_selfhost_main();

namespace bronze::runtime {

namespace {

constexpr size_t kCount = static_cast<size_t>(SelfHosted::Count);

// Each row's registry key, the specification's name for it: what the
// library's `$Builtin` rows say and the C++ tables ask for.
constexpr const char* kKeys[kCount] = {
    "",
    "Array.prototype.forEach",
    "Array.prototype.map",
    "Array.prototype.filter",
    "Array.prototype.some",
    "Array.prototype.every",
    "Array.prototype.find",
    "Array.prototype.findIndex",
    "Array.prototype.reduce",
    "Array.prototype.findLast",
    "Array.prototype.findLastIndex",
};

thread_local std::array<Value, kCount> t_functions{};
thread_local bool t_entered = false;

// The builtins' code, process-wide: one library image, so one code pointer
// per builtin whichever thread registered it first. The bounds make the
// common answer (any other code) two compares.
std::array<std::atomic<const void*>, kCount> g_codes{};
std::atomic<uintptr_t> g_codeLo{UINTPTR_MAX};
std::atomic<uintptr_t> g_codeHi{0};

void noteCode(size_t id, const void* code) {
    g_codes[id].store(code, std::memory_order_relaxed);
    const auto at = reinterpret_cast<uintptr_t>(code);
    uintptr_t lo = g_codeLo.load(std::memory_order_relaxed);
    while (at < lo && !g_codeLo.compare_exchange_weak(lo, at, std::memory_order_release)) {
    }
    uintptr_t hi = g_codeHi.load(std::memory_order_relaxed);
    while (at > hi && !g_codeHi.compare_exchange_weak(hi, at, std::memory_order_release)) {
    }
}

void runLibraryEntry() {
    if (t_entered) return;
    t_entered = true;
    rtRunAsLazyInit([] {
        if (!rtRunEntryForEveryRealm(&bronze_selfhost_main)) {
            fatal("self-hosted builtins: the library's entry registered no module instance");
        }
    });
}

}  // namespace

Value rtSelfHostedFunction(SelfHosted id) {
    const auto i = static_cast<size_t>(id);
    if (i == 0 || i >= kCount) fatal("rtSelfHostedFunction: no such builtin");
    if (t_functions[i].isUndefined()) runLibraryEntry();
    if (!t_functions[i].isObject()) {
        fatal((std::string("self-hosted builtins: the library registered no ") + kKeys[i]).c_str());
    }
    return t_functions[i];
}

bool rtIsSelfHostedCode(const void* code) noexcept {
    const auto at = reinterpret_cast<uintptr_t>(code);
    if (at < g_codeLo.load(std::memory_order_acquire) || at > g_codeHi.load(std::memory_order_acquire)) {
        return false;
    }
    for (size_t i = 1; i < kCount; ++i) {
        if (g_codes[i].load(std::memory_order_relaxed) == code) return true;
    }
    return false;
}

}  // namespace bronze::runtime

using namespace bronze;
using namespace bronze::runtime;

namespace {

// 7.1.18 ToObject's TypeError: the library's message, or the generic one.
std::string calledOnNullish(Value message) {
    if (message.isString()) {
        std::string text = rtUtf8Chars(message.asString<StringHeader>());
        if (!text.empty()) return text;
    }
    return "Cannot convert undefined or null to object";
}

double numberOf(Value v) { return v.isNumber() ? v.asNumber() : std::numeric_limits<double>::quiet_NaN(); }

// What V8 says a non-callable value is ("number 1 is not a function").
std::string describeNotCallable(Rooted<Value>& v) {
    const Value x = v.get();
    if (x.isUndefined()) return "undefined";
    if (x.isNull()) return "object null";
    if (x.isBool()) return x.asBool() ? "boolean true" : "boolean false";
    if (x.isString()) return "string \"" + rtUtf8Chars(x.asString<StringHeader>()) + "\"";
    if (x.isSymbol()) return "symbol";
    if (x.isObject()) return "object";
    if (x.isNumber()) {
        Rooted<Value> text{rtValueToString(x)};
        return "number " + rtUtf8Chars(text.get().asString<StringHeader>());
    }
    return "bigint";
}

}  // namespace

extern "C" {

// `$Builtin(fn, key, ...)`: one row of the library's registry.
uint64_t bronze_sh_register(uint64_t keyBits, uint64_t fnBits) {
    const Value key(keyBits);
    const Value fn(fnBits);
    if (!key.isString() || !fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        fatal("self-hosted builtins: $Builtin registered something that is not a function");
    }
    const std::string name = rtUtf8Chars(key.asString<StringHeader>());
    for (size_t i = 1; i < kCount; ++i) {
        if (name != kKeys[i]) continue;
        // A permanent root of the thread's heap, as every intrinsic is.
        if (t_functions[i].isUndefined()) rtHeap().add_permanent_root(&t_functions[i]);
        t_functions[i] = fn;
        auto* hdr = fn.asObject<FunctionHeader>();
        noteCode(i, reinterpret_cast<const void*>(hdr->code));
        return Value::fromUndefined().rawBits();
    }
    fatal(("self-hosted builtins: $Builtin names " + name + ", which runtime/selfhost.cpp does not list").c_str());
}

uint64_t bronze_sh_to_object(uint64_t vBits, uint64_t messageBits) {
    const Value v(vBits);
    if (v.isObject()) return vBits;
    if (v.isUndefined() || v.isNull()) return rtThrowTypeError(calledOnNullish(Value(messageBits))).rawBits();
    return rtToObject(v).rawBits();
}

uint64_t bronze_sh_throw_not_callable(uint64_t vBits) {
    Rooted<Value> v{Value(vBits)};
    return rtThrowTypeError(describeNotCallable(v) + " is not a function").rawBits();
}

uint64_t bronze_sh_throw_type_error(uint64_t msgBits) {
    const Value msg(msgBits);
    return rtThrowTypeError(msg.isString() ? rtUtf8Chars(msg.asString<StringHeader>()) : std::string())
        .rawBits();
}

uint64_t bronze_sh_throw_range_error(uint64_t msgBits) {
    const Value msg(msgBits);
    return rtThrowRangeError(msg.isString() ? rtUtf8Chars(msg.asString<StringHeader>()) : std::string())
        .rawBits();
}

// 10.4.2.3 ArraySpeciesCreate(O, length), `length` a Number the caller made
// with ToLength.
uint64_t bronze_sh_array_species_create(uint64_t oBits, uint64_t lenBits) {
    const double len = numberOf(Value(lenBits));
    if (!(len <= 4294967295.0)) return rtThrowRangeError("Invalid array length").rawBits();
    Rooted<Value> o{Value(oBits)};
    return rtArraySpeciesCreate(o, static_cast<uint32_t>(len)).rawBits();
}

// 7.3.18 LengthOfArrayLike(O): an Array's length off its header, anything
// else ToLength(Get(O, "length")).
uint64_t bronze_sh_length_of_array_like(uint64_t oBits) {
    const Value o(oBits);
    if (isArray(o)) return Value::fromDouble(o.asObject<ArrayHeader>()->length).rawBits();
    Rooted<Value> key{rtMakeString("length")};
    Rooted<Value> len{Value(bronze_elem_get(oBits, key.get().rawBits()))};
    if (len.get().isNumber()) {
        const double d = len.get().asNumber();
        if (d >= 0 && d <= 4294967295.0 && d == static_cast<double>(static_cast<uint32_t>(d))) {
            return Value::fromDouble(d + 0.0).rawBits();
        }
    }
    // 7.1.20 ToLength: ToIntegerOrInfinity clamped to [0, 2^53 - 1].
    const double n = rtToNumber(len.get());
    if (std::isnan(n) || n <= 0) return Value::fromDouble(0).rawBits();
    const double t = std::trunc(n);
    return Value::fromDouble(t < 9007199254740991.0 ? t : 9007199254740991.0).rawBits();
}

}  // extern "C"
