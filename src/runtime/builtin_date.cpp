// `Date` — the constructor (ECMA-262 21.4.2), the statics (21.4.3), and the
// `Date.prototype` object (21.4.4) the field accessors in
// builtin_date_accessors.cpp are installed onto.
//
// A Date is a PLAIN object with internal slots and a real prototype, which is
// what 21.4.4 says it is and what a Map deliberately is not. The difference is
// not stylistic: `d instanceof Date`, `Date.prototype.getTime.call(x)`,
// `Object.setPrototypeOf`, a subclass's `toString` override and
// `Date.prototype[Symbol.toPrimitive]` all need a holder a program can reach,
// and a table consulted beside the value has none. The wrapper objects
// (builtin_wrappers.cpp) already had this arrangement; this is the same one.
//
// The prototype chain of a Date is Date.prototype -> (the chain-end fallback
// that stands in for Object.prototype, rtObjectProtoMember) -> null, exactly as
// `Promise.prototype`'s instances are arranged.

#include <cmath>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <limits>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/builtin_date_internal.h"
#include "runtime/date.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

namespace dt = datetime;

thread_local Value g_dateCtor = Value::fromUndefined();
thread_local Value g_dateProto = Value::fromUndefined();
thread_local Shape* g_dateInstanceShape = nullptr;
// The brand. A symbol lives in the non-moving arena and is never collected
// (symbol.h), so this is an ordinary static rather than a GC root — and a
// program has no way to name it, which is what makes it unforgeable.
thread_local SymbolHeader* g_dateBrand = nullptr;

void ensureDateIntrinsics();

// 21.4.4.2.1's clock read. The determinism rule is about the COMPILER's output,
// not a compiled program's: a program that reads the clock reads the clock,
// exactly as it would under node, and an oracle case that printed this value
// would be wrong for a reason no fixed constant could fix.
double nowMs() {
    const auto since = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(since).count());
}

// Did `new` build this receiver? Both construct paths — `bronze_construct` and
// the inline `new` fast path generated code takes once the constructor is
// vetted — allocate the instance from `fn->instance_shape`, and nothing else in
// the program produces an object with that shape: `rtNewRootShape` mints a
// fresh node, and `Object.create(Date.prototype)` goes through the memoized
// `rtRootShapeForPrototype`, which is a different one.
//
// The NEW-TARGET scope is deliberately NOT consulted. The inline path does not
// push one (llvm_construct.h says so, and says its soundness rests on
// `bronze_get_new_target` being that scope's only observer), so a second
// observer here would read whatever an enclosing `new` had left behind.
bool builtByNew(Value thisVal) {
    if (!thisVal.isObject()) return false;
    HeapObjectHeader* hdr = thisVal.asObject<HeapObjectHeader>();
    if (hdr->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) return false;
    const auto* obj = reinterpret_cast<const ObjectHeader*>(hdr);
    return obj->shape && obj->shape->root == g_dateInstanceShape;
}

// ---- the constructor --------------------------------------------------------

// 21.4.2.1 step 3.a: a Date argument is copied by its [[DateValue]] and NOT by
// running ToPrimitive over it, so `new Date(d)` is exact even for a `d` whose
// `valueOf` a program replaced.
double timeValueOfArgument(Rooted<Value>& arg) {
    if (rtIsDateObject(arg.get())) {
        return arg.get().asObject<ObjectHeader>()->internalSlot(DateSlot::TimeValue).asNumber();
    }
    Rooted<Value> prim{rtToPrimitive(arg, ToPrimitiveHint::Default)};
    if (prim.get().isString()) {
        // 21.4.2.1 step 4.b: the same parse as Date.parse, so `new Date(s)`
        // and `new Date(Date.parse(s))` are the same Date.
        return dt::parse(rtUtf8Chars(prim.get().asString<StringHeader>()));
    }
    return rtToNumber(prim.get());
}

// 21.4.2.1 steps 4-8: the field form. Every argument is converted with ToNumber
// in ARGUMENT ORDER before any of them is used, which is observable through a
// `valueOf` with a side effect.
double timeValueOfFields(RootedArgs& args, bool local) {
    // The DEFAULTS 21.4.2.1 step 6 and 21.4.3.4 name for an omitted field: the
    // date is 1 and everything else is +0.
    double parts[7] = {0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0};
    // The YEAR is converted whether or not it was passed — `Date.UTC()` is
    // ToNumber(undefined), which is NaN, and therefore an invalid date rather
    // than the year 1900 an unconverted +0 would have produced. Only the fields
    // after it are conditional.
    parts[0] = rtToNumber(args[0]);
    const uint32_t supplied = args.count() < 7 ? args.count() : 7;
    for (uint32_t i = 1; i < supplied; ++i) {
        parts[i] = rtToNumber(args[i]);
    }
    const double year = dt::makeFullYear(parts[0]);
    const double when = dt::makeDate(dt::makeDay(year, parts[1], parts[2]),
                                     dt::makeTime(parts[3], parts[4], parts[5], parts[6]));
    return dt::timeClip(local ? dt::utcFromLocal(when) : when);
}

uint64_t dateConstructor(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    ensureDateIntrinsics();
    RootedArgs args(argc, argv);
    // 21.4.2.2: called as a FUNCTION, `Date` ignores every argument and answers
    // the current time as a String — the same bytes `toString` would produce.
    // It is not `new Date(...).toString()`: the arguments are not even read.
    if (!builtByNew(Value(thisBits))) {
        return rtMakeString(dt::dateTimeString(nowMs())).rawBits();
    }

    double tv = 0.0;
    if (argc == 0) {
        tv = nowMs();
    } else if (argc == 1) {
        Rooted<Value> arg{args[0]};
        tv = dt::timeClip(timeValueOfArgument(arg));
    } else {
        tv = timeValueOfFields(args, /*local=*/true);
    }
    return rtMakeDateObject(tv).rawBits();
}

// ---- the statics (21.4.3) ---------------------------------------------------

uint64_t dateNow(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return Value::fromDouble(nowMs()).rawBits();
}

// 21.4.3.4. The same field algorithm as the constructor's, read as UTC — which
// is the whole difference between the two and the reason it is one function.
uint64_t dateUTC(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    const double tv = timeValueOfFields(args, /*local=*/false);
    return Value::fromDouble(tv).rawBits();
}

// 21.4.3.2.
uint64_t dateParse(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> arg{args[0]};
    Rooted<Value> str{rtToStringValue(arg)};
    return Value::fromDouble(dt::parse(rtUtf8Chars(str.get().asString<StringHeader>()))).rawBits();
}

// ---- the string members -----------------------------------------------------

// One body for the four that differ only in which format they ask for.
using Formatter = std::string (*)(double);

template <Formatter Format>
uint64_t dateToText(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    double t = 0.0;
    if (!rtDateThisTimeValue(Value(thisBits), "a Date string method", t)) {
        return Value::fromUndefined().rawBits();
    }
    return rtMakeString(Format(t)).rawBits();
}

// 21.4.4.36. The RangeError is THROWN and not fataled: the clause names it, so
// a program may catch it — which is exactly what `try { d.toISOString() }`
// around a possibly-invalid date does.
uint64_t dateToISOString(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    double t = 0.0;
    if (!rtDateThisTimeValue(Value(thisBits), "toISOString", t)) {
        return Value::fromUndefined().rawBits();
    }
    std::string text;
    if (!dt::isoString(t, text)) {
        return rtThrowRangeError("Invalid time value").rawBits();
    }
    return rtMakeString(text).rawBits();
}

// 21.4.4.37 toJSON. Note what it is NOT: it does not read [[DateValue]], so it
// works on any object — the number probe is a ToPrimitive with hint NUMBER over
// the RECEIVER, and the answer comes from whatever `toISOString` that receiver
// has. That is what makes it overridable, and it is why `JSON.stringify` of an
// invalid Date is "null" rather than a thrown RangeError: this catches the
// non-finite case one step before `toISOString` would raise.
uint64_t dateToJSON(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    if (!self.get().isObject()) {
        return rtThrowTypeError("Date.prototype.toJSON called on a value that is not an object")
            .rawBits();
    }
    Rooted<Value> prim{rtToPrimitive(self, ToPrimitiveHint::Number)};
    if (prim.get().isNumber() && !std::isfinite(prim.get().asNumber())) {
        return Value::fromNull().rawBits();
    }
    Rooted<Value> key{rtMakeString("toISOString")};
    Rooted<Value> method{Value(bronze_elem_get(self.get().rawBits(), key.get().rawBits()))};
    if (!method.get().isObject() ||
        method.get().asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return rtThrowTypeError("Date.prototype.toJSON: toISOString is not a function").rawBits();
    }
    return bronze_dynamic_call(method.get().rawBits(), self.get().rawBits(), 0, nullptr);
}

// 21.4.4.45 `Date.prototype[Symbol.toPrimitive]`, and the one place in the
// language where hint DEFAULT behaves as hint STRING. That single line is why
// `date + 1` concatenates while `date - date` subtracts: `+` asks with no hint
// and lands in the string branch, `-` asks for a number and does not.
uint64_t dateToPrimitive(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!self.get().isObject()) {
        return rtThrowTypeError(
                   "Date.prototype[Symbol.toPrimitive] called on a value that is not an object")
            .rawBits();
    }
    if (!args[0].isString()) {
        return rtThrowTypeError("Date.prototype[Symbol.toPrimitive] needs a string hint")
            .rawBits();
    }
    const std::string hint = rtUtf8Chars(args[0].asString<StringHeader>());
    ToPrimitiveHint tryFirst;
    if (hint == "string" || hint == "default") {
        tryFirst = ToPrimitiveHint::String;
    } else if (hint == "number") {
        tryFirst = ToPrimitiveHint::Number;
    } else {
        return rtThrowTypeError("Date.prototype[Symbol.toPrimitive]: invalid hint " + hint)
            .rawBits();
    }
    // OrdinaryToPrimitive and not the whole of 7.1.1: step 2 of that algorithm
    // is the lookup that found THIS function, so going back through it would
    // recurse until the stack ran out.
    return rtOrdinaryToPrimitive(self, tryFirst).rawBits();
}

uint64_t dateGetYear(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    double t = 0.0;
    if (!rtDateThisTimeValue(Value(thisBits), "getYear", t)) {
        return Value::fromUndefined().rawBits();
    }
    if (std::isnan(t)) return Value::fromDouble(t).rawBits();
    return Value::fromDouble(dt::yearFromTime(dt::localTime(t)) - 1900.0).rawBits();
}

uint64_t dateSetYear(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    double t = 0.0;
    if (!rtDateThisTimeValue(self.get(), "setYear", t)) {
        return Value::fromUndefined().rawBits();
    }
    double y = args.count() > 0 ? rtToNumber(args[0]) : std::numeric_limits<double>::quiet_NaN();
    double zoned = std::isnan(t) ? 0.0 : dt::localTime(t);
    y = dt::makeFullYear(y);
    const double day = dt::makeDay(y, dt::monthFromTime(zoned), dt::dateFromTime(zoned));
    const double when = dt::makeDate(day, dt::timeWithinDay(zoned));
    const double u = dt::timeClip(dt::utcFromLocal(when));
    rtDateSetTimeValue(self.get(), u);
    return Value::fromDouble(u).rawBits();
}

const NativeMethod kStringMembers[] = {
    {"toString", dateToText<dt::dateTimeString>, 0, 0},
    {"toDateString", dateToText<dt::dateOnlyString>, 0, 0},
    {"toTimeString", dateToText<dt::timeOnlyString>, 0, 0},
    {"toUTCString", dateToText<dt::utcString>, 0, 0},
    {"toISOString", dateToISOString, 0, 0},
    {"toJSON", dateToJSON, 0, 1},
    {"toLocaleString", dateToText<dt::localeDateTimeString>, 0, 0},
    {"toLocaleDateString", dateToText<dt::localeDateString>, 0, 0},
    {"toLocaleTimeString", dateToText<dt::localeTimeString>, 0, 0},
    {"getYear", dateGetYear, 0, 0},
    {"setYear", dateSetYear, 0, 1},
    // B.2.3.3: `toGMTString` IS `toUTCString` — the same function object,
    // named "toUTCString" — which the code-pointer interning gives once the
    // row names the same body. Listed AFTER it so the canonical name creates.
    {"toGMTString", dateToText<dt::utcString>, 0, 0},
};

// ---- assembling the intrinsics ----------------------------------------------

void ensureDateIntrinsics() {
    if (g_dateProto.isObject()) return;

    // 21.4.4: `Date.prototype`'s [[Prototype]] is `Object.prototype`, and the
    // link is REAL — the arrangement builtin_wrappers.cpp uses, not the
    // chain-end one `Promise.prototype` has. It has to be: `Object.prototype`
    // carries a `toLocaleString` (20.1.3.5) that forwards to `toString`, so a
    // Date reaching it through a walk that stopped short would have answered
    // `undefined` for `hasOwnProperty` while a chain that reached it would have
    // answered 20.1.3.5 where 21.4.4.39 belongs. Both are wrong, and only the
    // real link plus this file's own named refusal for the three locale members
    // gets both right.
    Rooted<Value> parent{rtObjectPrototype()};
    Shape* protoShape = rtNewRootShape(parent.get());
    protoShape->used_as_prototype = true;
    ObjectHeader* protoObj = ObjectHeader::create(rtHeap(), rtArena(), protoShape);
    protoObj->header.flags = HeapKind::Plain;
    Rooted<Value> proto{Value::fromObject(protoObj)};

    // Published before anything is installed, and as a permanent root: every
    // install below allocates, and the collector moves this object.
    g_dateProto = proto.get();
    rtHeap().add_permanent_root(&g_dateProto);

    size_t accessorCount = 0;
    const NativeMethod* accessors = rtDateAccessorMethods(accessorCount);
    rtDefineMethods(proto, accessors, accessorCount);
    rtDefineMethods(proto, kStringMembers, std::size(kStringMembers));

    {
        // 21.4.4.45's key is a symbol, so it cannot ride the string-keyed table
        // above — but it is defined on the same terms: non-enumerable, and a
        // DEFINITION rather than an assignment.
        Rooted<Value> key{Value::fromSymbol(rtSymbolToPrimitive())};
        Rooted<Value> val{rtNativeFunction(dateToPrimitive, 0, "[Symbol.toPrimitive]", 1)};
        proto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
    }

    // 21.4.3: `Date` has length 7, the seven fields of the component form.
    Rooted<Value> ctor{rtNativeFunction(dateConstructor, 0, "Date", 7)};
    rtEnsureFunctionProperties(ctor);
    Rooted<Value> props{ctor.get().asObject<FunctionHeader>()->properties};
    const NativeMethod statics[] = {
        {"now", dateNow, 0, 0},
        {"parse", dateParse, 0, 1},
        {"UTC", dateUTC, 0, 7},
    };
    for (const NativeMethod& s : statics) {
        Rooted<Value> key{rtMakeString(s.name)};
        Rooted<Value> val{rtNativeFunction(s.code, s.arity, s.name, s.length)};
        props.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
    }

    // 21.4.4.1: the back-pointer, a DEFINITION so it does not write through.
    {
        Rooted<Value> key{rtMakeString("constructor")};
        proto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, ctor, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
    }

    // The brand symbol. Minted with no description, because nothing prints it:
    // it exists to be compared by identity and a program can never hold it.
    g_dateBrand = rtMakeSymbol(Value::fromUndefined()).asSymbol<SymbolHeader>();

    FunctionHeader* fn = ctor.get().asObject<FunctionHeader>();
    fn->prototype = proto.get();
    // Last: `builtByNew` tests against this, and an instance cannot exist
    // before there is a shape to build one from.
    fn->instance_shape = rtNewRootShape(proto.get());
    g_dateInstanceShape = fn->instance_shape;
    g_dateCtor = ctor.get();
    rtHeap().add_permanent_root(&g_dateCtor);
}

}  // namespace

bool rtIsDateObject(Value v) {
    if (!g_dateBrand || !v.isObject()) return false;
    HeapObjectHeader* hdr = v.asObject<HeapObjectHeader>();
    if (hdr->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) return false;
    auto* obj = reinterpret_cast<ObjectHeader*>(hdr);
    if (obj->internalSlotCount() != DateSlot::kCount) return false;
    const Value brand = obj->internalSlot(DateSlot::Brand);
    return brand.isSymbol() && brand.asSymbol<SymbolHeader>() == g_dateBrand;
}

Value rtMakeDateObject(double t) {
    ensureDateIntrinsics();
    ObjectHeader* obj = ObjectHeader::createWithInternalSlots(rtHeap(), rtArena(),
                                                              g_dateInstanceShape,
                                                              DateSlot::kCount);
    obj->header.flags = BRONZE_ABI_OBJ_FLAGS_PLAIN;
    obj->setInternalSlot(DateSlot::TimeValue, Value::fromDouble(t));
    obj->setInternalSlot(DateSlot::Brand, Value::fromSymbol(g_dateBrand));
    return Value::fromObject(obj);
}

bool rtDateThisTimeValue(Value self, const char* method, double& out) {
    if (!rtIsDateObject(self)) {
        rtThrowTypeError(std::string("Date.prototype.") + method +
                         " called on a receiver that is not a Date");
        return false;
    }
    out = self.asObject<ObjectHeader>()->internalSlot(DateSlot::TimeValue).asNumber();
    return true;
}

void rtDateSetTimeValue(Value self, double t) {
    self.asObject<ObjectHeader>()->setInternalSlot(DateSlot::TimeValue, Value::fromDouble(t));
}

Value rtDateConstructor() {
    ensureDateIntrinsics();
    return g_dateCtor;
}

bool rtIsDateConstructor(Value fn) {
    return g_dateCtor.isObject() && fn.rawBits() == g_dateCtor.rawBits();
}

bool rtDateInspectText(Value v, std::string& out) {
    if (!rtIsDateObject(v)) return false;
    out = dt::inspectString(
        v.asObject<ObjectHeader>()->internalSlot(DateSlot::TimeValue).asNumber());
    return true;
}

}  // namespace bronze::runtime
