// The `Intl` namespace (ECMA-402) and `Intl.DateTimeFormat` with IANA / offset
// `timeZone` support.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/date.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/number_format.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

namespace dt = datetime;

namespace DtfSlot {
enum : uint32_t {
    Brand = 0,
    Locale = 1,
    TimeZone = 2,
    Weekday = 3,
    Year = 4,
    Month = 5,
    Day = 6,
    Hour = 7,
    Minute = 8,
    Second = 9,
    TimeZoneName = 10,
    DateStyle = 11,
    TimeStyle = 12,
    Hour12 = 13,
    HourCycle = 14,
    kCount = 15,
};
}  // namespace DtfSlot

thread_local Value g_intlObject = Value::fromUndefined();
thread_local Value g_dtfCtor = Value::fromUndefined();
thread_local Value g_dtfProto = Value::fromUndefined();
thread_local Shape* g_dtfInstanceShape = nullptr;
thread_local SymbolHeader* g_dtfBrand = nullptr;

void ensureIntlIntrinsics();

double nowMs() {
    const auto since = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(since).count());
}

bool isDateTimeFormatObject(Value v) {
    if (!g_dtfBrand || !v.isObject()) return false;
    HeapObjectHeader* hdr = v.asObject<HeapObjectHeader>();
    if (hdr->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) return false;
    auto* obj = reinterpret_cast<ObjectHeader*>(hdr);
    if (obj->internalSlotCount() != DtfSlot::kCount) return false;
    const Value brand = obj->internalSlot(DtfSlot::Brand);
    return brand.isSymbol() && brand.asSymbol<SymbolHeader>() == g_dtfBrand;
}

Value getOptionProp(Rooted<Value>& optsObj, const char* name) {
    Rooted<Value> key{rtMakeString(name)};
    return Value(bronze_elem_get(optsObj.get().rawBits(), key.get().rawBits()));
}

bool readStringOption(Rooted<Value>& optsObj, const char* name,
                      std::initializer_list<const char*> allowed, std::string& out) {
    Rooted<Value> val{getOptionProp(optsObj, name)};
    if (val.get().isUndefined()) return true;
    Rooted<Value> strVal{rtToStringValue(val)};
    std::string s = rtUtf8Chars(strVal.get().asString<StringHeader>());
    if (allowed.size() > 0) {
        bool match = false;
        for (const char* a : allowed) {
            if (s == a) {
                match = true;
                break;
            }
        }
        if (!match) {
            rtThrowRangeError(std::string("Value ") + s + " out of range for Intl.DateTimeFormat options property " + name);
            return false;
        }
    }
    out = std::move(s);
    return true;
}

std::string resolveLocaleArg(Value localesArg) {
    if (localesArg.isUndefined()) return "en-US";
    if (localesArg.isString()) {
        std::string s = rtUtf8Chars(localesArg.asString<StringHeader>());
        return s.empty() ? "en-US" : s;
    }
    if (localesArg.isObject()) {
        Rooted<Value> obj{localesArg};
        Rooted<Value> zeroKey{rtMakeString("0")};
        Rooted<Value> first{Value(bronze_elem_get(obj.get().rawBits(), zeroKey.get().rawBits()))};
        if (first.get().isString()) {
            std::string s = rtUtf8Chars(first.get().asString<StringHeader>());
            if (!s.empty()) return s;
        }
    }
    return "en-US";
}

bool parseDateTimeFormatOptions(uint32_t argc, const uint64_t* argv, bool defaultDate,
                                bool defaultTime, bool useDefaultZoneWhenOmitted,
                                dt::DateTimeFormatOptions& out) {
    RootedArgs args(argc, argv);
    out.locale = (argc > 0) ? resolveLocaleArg(args[0]) : "en-US";

    bool explicitTimeZone = false;
    if (argc > 1 && !args[1].isUndefined()) {
        if (args[1].isNull()) {
            rtThrowTypeError("Intl.DateTimeFormat options cannot be null");
            return false;
        }
        Rooted<Value> optsObj{rtToObject(args[1])};

        Rooted<Value> tzVal{getOptionProp(optsObj, "timeZone")};
        if (!tzVal.get().isUndefined()) {
            Rooted<Value> tzStrVal{rtToStringValue(tzVal)};
            const std::string rawTz = rtUtf8Chars(tzStrVal.get().asString<StringHeader>());
            std::string canonicalTz;
            if (!dt::resolveTimeZone(rawTz, canonicalTz)) {
                rtThrowRangeError("Invalid time zone specified: " + rawTz);
                return false;
            }
            out.timeZone = std::move(canonicalTz);
            explicitTimeZone = true;
        }

        if (!readStringOption(optsObj, "weekday", {"narrow", "short", "long"}, out.weekday)) return false;
        if (!readStringOption(optsObj, "year", {"2-digit", "numeric"}, out.year)) return false;
        if (!readStringOption(optsObj, "month", {"2-digit", "numeric", "narrow", "short", "long"}, out.month)) return false;
        if (!readStringOption(optsObj, "day", {"2-digit", "numeric"}, out.day)) return false;
        if (!readStringOption(optsObj, "hour", {"2-digit", "numeric"}, out.hour)) return false;
        if (!readStringOption(optsObj, "minute", {"2-digit", "numeric"}, out.minute)) return false;
        if (!readStringOption(optsObj, "second", {"2-digit", "numeric"}, out.second)) return false;
        if (!readStringOption(optsObj, "timeZoneName",
                              {"short", "long", "shortOffset", "longOffset", "shortGeneric", "longGeneric"},
                              out.timeZoneName)) return false;
        if (!readStringOption(optsObj, "dateStyle", {"full", "long", "medium", "short"}, out.dateStyle)) return false;
        if (!readStringOption(optsObj, "timeStyle", {"full", "long", "medium", "short"}, out.timeStyle)) return false;
        if (!readStringOption(optsObj, "hourCycle", {"h11", "h12", "h23", "h24"}, out.hourCycle)) return false;

        Rooted<Value> h12Val{getOptionProp(optsObj, "hour12")};
        if (!h12Val.get().isUndefined()) {
            out.hasHour12 = true;
            out.hour12 = bronze_truthy(h12Val.get().rawBits());
            out.hourCycle = out.hour12 ? "h12" : "h23";
        } else if (!out.hourCycle.empty()) {
            out.hour12 = (out.hourCycle == "h11" || out.hourCycle == "h12");
        }
    }

    if (!explicitTimeZone && useDefaultZoneWhenOmitted) {
        out.timeZone = dt::defaultTimeZone();
    }

    dt::applyDateTimeDefaults(out, defaultDate, defaultTime);
    if (!out.hour.empty() && out.hourCycle.empty()) {
        out.hourCycle = out.hour12 ? "h12" : "h23";
    }
    return true;
}

Value slotStringOrUndefined(const std::string& s) {
    return s.empty() ? Value::fromUndefined() : rtMakeString(s);
}

std::string stringFromSlot(Value v) {
    if (!v.isString()) return {};
    return rtUtf8Chars(v.asString<StringHeader>());
}

dt::DateTimeFormatOptions readOptionsFromInstance(Value dtfVal) {
    auto* obj = dtfVal.asObject<ObjectHeader>();
    dt::DateTimeFormatOptions opts;
    opts.locale = stringFromSlot(obj->internalSlot(DtfSlot::Locale));
    opts.timeZone = stringFromSlot(obj->internalSlot(DtfSlot::TimeZone));
    opts.weekday = stringFromSlot(obj->internalSlot(DtfSlot::Weekday));
    opts.year = stringFromSlot(obj->internalSlot(DtfSlot::Year));
    opts.month = stringFromSlot(obj->internalSlot(DtfSlot::Month));
    opts.day = stringFromSlot(obj->internalSlot(DtfSlot::Day));
    opts.hour = stringFromSlot(obj->internalSlot(DtfSlot::Hour));
    opts.minute = stringFromSlot(obj->internalSlot(DtfSlot::Minute));
    opts.second = stringFromSlot(obj->internalSlot(DtfSlot::Second));
    opts.timeZoneName = stringFromSlot(obj->internalSlot(DtfSlot::TimeZoneName));
    opts.dateStyle = stringFromSlot(obj->internalSlot(DtfSlot::DateStyle));
    opts.timeStyle = stringFromSlot(obj->internalSlot(DtfSlot::TimeStyle));
    const Value h12 = obj->internalSlot(DtfSlot::Hour12);
    if (h12.isBool()) {
        opts.hasHour12 = true;
        opts.hour12 = h12.asBool();
    }
    opts.hourCycle = stringFromSlot(obj->internalSlot(DtfSlot::HourCycle));
    if (!opts.hourCycle.empty() && !opts.hasHour12) {
        opts.hour12 = (opts.hourCycle == "h11" || opts.hourCycle == "h12");
    }
    return opts;
}

bool extractFormatTimeValue(uint32_t argc, const uint64_t* argv, double& tvOut) {
    if (argc == 0 || Value(argv[0]).isUndefined()) {
        tvOut = nowMs();
        return true;
    }
    Value arg(argv[0]);
    if (rtIsDateObject(arg)) {
        tvOut = arg.asObject<ObjectHeader>()->internalSlot(0).asNumber();
    } else {
        tvOut = rtToNumber(arg);
    }
    tvOut = dt::timeClip(tvOut);
    if (std::isnan(tvOut)) {
        rtThrowRangeError("Invalid time value");
        return false;
    }
    return true;
}

uint64_t dtfFormatMethod(uint64_t envBits, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    Value target(thisBits);
    if (!isDateTimeFormatObject(target)) {
        Value boundEnv(envBits);
        if (isDateTimeFormatObject(boundEnv)) {
            target = boundEnv;
        } else {
            return rtThrowTypeError(
                       "Intl.DateTimeFormat.prototype.format called on incompatible receiver")
                .rawBits();
        }
    }
    Rooted<Value> dtfRoot{target};
    double tv = 0.0;
    if (!extractFormatTimeValue(argc, argv, tv)) {
        return Value::fromUndefined().rawBits();
    }
    const dt::DateTimeFormatOptions opts = readOptionsFromInstance(dtfRoot.get());
    std::string formatted;
    if (!dt::formatDateTime(tv, opts, formatted)) {
        return rtThrowRangeError("Invalid time value").rawBits();
    }
    return rtMakeString(formatted).rawBits();
}

uint64_t dtfFormatToParts(uint64_t envBits, uint64_t thisBits, uint32_t argc,
                          const uint64_t* argv) {
    Value target(thisBits);
    if (!isDateTimeFormatObject(target)) {
        Value boundEnv(envBits);
        if (isDateTimeFormatObject(boundEnv)) {
            target = boundEnv;
        } else {
            return rtThrowTypeError(
                       "Intl.DateTimeFormat.prototype.formatToParts called on incompatible receiver")
                .rawBits();
        }
    }
    Rooted<Value> dtfRoot{target};
    double tv = 0.0;
    if (!extractFormatTimeValue(argc, argv, tv)) {
        return Value::fromUndefined().rawBits();
    }
    const dt::DateTimeFormatOptions opts = readOptionsFromInstance(dtfRoot.get());
    std::vector<dt::DateTimePart> parts;
    if (!dt::formatDateTimeParts(tv, opts, parts)) {
        return rtThrowRangeError("Invalid time value").rawBits();
    }
    const uint32_t n = static_cast<uint32_t>(parts.size());
    Rooted<Value> arr{Value::fromObject(ArrayHeader::create(rtHeap(), n))};
    for (uint32_t i = 0; i < n; ++i) {
        Rooted<Value> item{Value::fromObject(
            ObjectHeader::create(rtHeap(), rtArena(), rtPlainObjectShape()))};
        item.get().asObject<ObjectHeader>()->header.flags = HeapKind::Plain;
        Rooted<Value> typeKey{rtMakeString("type")};
        Rooted<Value> typeVal{rtMakeString(parts[i].type)};
        item.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), typeKey, typeVal);
        Rooted<Value> valKey{rtMakeString("value")};
        Rooted<Value> valVal{rtMakeString(parts[i].value)};
        item.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), valKey, valVal);
        arr.get().asObject<ArrayHeader>()->setElem(rtHeap(), i, item);
    }
    return arr.get().rawBits();
}

uint64_t dtfResolvedOptions(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Value self(thisBits);
    if (!isDateTimeFormatObject(self)) {
        return rtThrowTypeError(
                   "Intl.DateTimeFormat.prototype.resolvedOptions called on incompatible receiver")
            .rawBits();
    }
    const dt::DateTimeFormatOptions opts = readOptionsFromInstance(self);
    Rooted<Value> res{Value::fromObject(
        ObjectHeader::create(rtHeap(), rtArena(), rtPlainObjectShape()))};
    res.get().asObject<ObjectHeader>()->header.flags = HeapKind::Plain;

    auto setStr = [&](const char* k, const std::string& v) {
        if (v.empty()) return;
        Rooted<Value> key{rtMakeString(k)};
        Rooted<Value> val{rtMakeString(v)};
        res.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
    };

    setStr("locale", opts.locale.empty() ? "en-US" : opts.locale);
    setStr("calendar", "gregory");
    setStr("numberingSystem", "latn");
    setStr("timeZone", opts.timeZone.empty() ? dt::defaultTimeZone() : opts.timeZone);
    if (!opts.hour.empty() || opts.hasHour12 || !opts.hourCycle.empty()) {
        Rooted<Value> h12Key{rtMakeString("hour12")};
        Rooted<Value> h12Val{Value::fromBool(opts.hour12)};
        res.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), h12Key, h12Val);
        setStr("hourCycle", opts.hourCycle.empty() ? (opts.hour12 ? "h12" : "h23") : opts.hourCycle);
    }
    setStr("weekday", opts.weekday);
    setStr("year", opts.year);
    setStr("month", opts.month);
    setStr("day", opts.day);
    setStr("hour", opts.hour);
    setStr("minute", opts.minute);
    setStr("second", opts.second);
    setStr("timeZoneName", opts.timeZoneName);
    setStr("dateStyle", opts.dateStyle);
    setStr("timeStyle", opts.timeStyle);
    return res.get().rawBits();
}

uint64_t dtfSupportedLocalesOf(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    Rooted<Value> arr{Value::fromObject(ArrayHeader::create(rtHeap(), 1))};
    Rooted<Value> enUs{rtMakeString("en-US")};
    arr.get().asObject<ArrayHeader>()->setElem(rtHeap(), 0, enUs);
    return arr.get().rawBits();
}

uint64_t dtfConstructor(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    ensureIntlIntrinsics();
    dt::DateTimeFormatOptions opts;
    if (!parseDateTimeFormatOptions(argc, argv, /*defaultDate=*/true, /*defaultTime=*/false,
                                    /*useDefaultZoneWhenOmitted=*/true, opts)) {
        return Value::fromUndefined().rawBits();
    }

    Rooted<Value> inst{Value::fromObject(ObjectHeader::createWithInternalSlots(
        rtHeap(), rtArena(), g_dtfInstanceShape, DtfSlot::kCount))};
    inst.get().asObject<ObjectHeader>()->header.flags = BRONZE_ABI_OBJ_FLAGS_PLAIN;
    inst.get().asObject<ObjectHeader>()->setInternalSlot(DtfSlot::Brand,
                                                         Value::fromSymbol(g_dtfBrand));

    auto setSlotStr = [&](uint32_t slot, const std::string& s) {
        Rooted<Value> val{slotStringOrUndefined(s)};
        inst.get().asObject<ObjectHeader>()->setInternalSlot(slot, val.get());
    };

    setSlotStr(DtfSlot::Locale, opts.locale);
    setSlotStr(DtfSlot::TimeZone, opts.timeZone);
    setSlotStr(DtfSlot::Weekday, opts.weekday);
    setSlotStr(DtfSlot::Year, opts.year);
    setSlotStr(DtfSlot::Month, opts.month);
    setSlotStr(DtfSlot::Day, opts.day);
    setSlotStr(DtfSlot::Hour, opts.hour);
    setSlotStr(DtfSlot::Minute, opts.minute);
    setSlotStr(DtfSlot::Second, opts.second);
    setSlotStr(DtfSlot::TimeZoneName, opts.timeZoneName);
    setSlotStr(DtfSlot::DateStyle, opts.dateStyle);
    setSlotStr(DtfSlot::TimeStyle, opts.timeStyle);
    if (!opts.hour.empty() || opts.hasHour12 || !opts.hourCycle.empty()) {
        inst.get().asObject<ObjectHeader>()->setInternalSlot(DtfSlot::Hour12,
                                                             Value::fromBool(opts.hour12));
    } else {
        inst.get().asObject<ObjectHeader>()->setInternalSlot(DtfSlot::Hour12,
                                                             Value::fromUndefined());
    }
    setSlotStr(DtfSlot::HourCycle, opts.hourCycle);

    // Install a bound `format` function on the instance so `const f = dtf.format; f(d)`
    // works even without `this`, while `dtf.format(d)` also works directly.
    FunctionHeader* boundFn = FunctionHeader::create(
        rtHeap(), dtfFormatMethod, inst.get(), 1, BRONZE_ABI_FN_FLAG_NATIVE);
    boundFn->header.flags = HeapKind::Function;
    Rooted<Value> fnVal{Value::fromObject(boundFn)};
    Rooted<Value> fmtKey{rtMakeString("format")};
    inst.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), fmtKey, fnVal, nullptr,
                                                 /*enumerable=*/false, /*defineOwn=*/true);

    return inst.get().rawBits();
}

// Minimal Intl.NumberFormat / Collator / PluralRules support so libraries that
// probe `Intl` find standard constructors alongside `Intl.DateTimeFormat`.
uint64_t numberFormatFormat(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    const double n = argc > 0 ? rtToNumber(Value(argv[0])) : std::nan("");
    return rtPrimitiveToString(Value::fromDouble(n)).rawBits();
}

uint64_t numberFormatResolvedOptions(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    Rooted<Value> res{Value::fromObject(
        ObjectHeader::create(rtHeap(), rtArena(), rtPlainObjectShape()))};
    res.get().asObject<ObjectHeader>()->header.flags = HeapKind::Plain;
    Rooted<Value> locKey{rtMakeString("locale")};
    Rooted<Value> locVal{rtMakeString("en-US")};
    res.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), locKey, locVal);
    Rooted<Value> numKey{rtMakeString("numberingSystem")};
    Rooted<Value> numVal{rtMakeString("latn")};
    res.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), numKey, numVal);
    return res.get().rawBits();
}

uint64_t numberFormatCtor(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    Rooted<Value> obj{Value::fromObject(
        ObjectHeader::create(rtHeap(), rtArena(), rtPlainObjectShape()))};
    obj.get().asObject<ObjectHeader>()->header.flags = HeapKind::Plain;
    Rooted<Value> fmtKey{rtMakeString("format")};
    Rooted<Value> fmtVal{rtNativeFunction(numberFormatFormat, 0, "format", 1)};
    obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), fmtKey, fmtVal, nullptr,
                                                /*enumerable=*/false, /*defineOwn=*/true);
    Rooted<Value> roKey{rtMakeString("resolvedOptions")};
    Rooted<Value> roVal{rtNativeFunction(numberFormatResolvedOptions, 0, "resolvedOptions", 0)};
    obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), roKey, roVal, nullptr,
                                                /*enumerable=*/false, /*defineOwn=*/true);
    return obj.get().rawBits();
}

uint64_t collatorCompare(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> a{args[0]};
    Rooted<Value> b{args[1]};
    Rooted<Value> sa{rtToStringValue(a)};
    Rooted<Value> sb{rtToStringValue(b)};
    const std::string ua = rtUtf8Chars(sa.get().asString<StringHeader>());
    const std::string ub = rtUtf8Chars(sb.get().asString<StringHeader>());
    const int cmp = ua < ub ? -1 : (ua > ub ? 1 : 0);
    return Value::fromDouble(cmp).rawBits();
}

uint64_t collatorCtor(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    Rooted<Value> obj{Value::fromObject(
        ObjectHeader::create(rtHeap(), rtArena(), rtPlainObjectShape()))};
    obj.get().asObject<ObjectHeader>()->header.flags = HeapKind::Plain;
    Rooted<Value> cmpKey{rtMakeString("compare")};
    Rooted<Value> cmpVal{rtNativeFunction(collatorCompare, 0, "compare", 2)};
    obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), cmpKey, cmpVal, nullptr,
                                                /*enumerable=*/false, /*defineOwn=*/true);
    Rooted<Value> roKey{rtMakeString("resolvedOptions")};
    Rooted<Value> roVal{rtNativeFunction(numberFormatResolvedOptions, 0, "resolvedOptions", 0)};
    obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), roKey, roVal, nullptr,
                                                /*enumerable=*/false, /*defineOwn=*/true);
    return obj.get().rawBits();
}

uint64_t getCanonicalLocales(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    if (argc == 0 || Value(argv[0]).isUndefined()) {
        return Value::fromObject(ArrayHeader::create(rtHeap(), 0)).rawBits();
    }
    const std::string loc = resolveLocaleArg(Value(argv[0]));
    Rooted<Value> arr{Value::fromObject(ArrayHeader::create(rtHeap(), 1))};
    Rooted<Value> item{rtMakeString(loc)};
    arr.get().asObject<ArrayHeader>()->setElem(rtHeap(), 0, item);
    return arr.get().rawBits();
}

void ensureIntlIntrinsics() {
    if (g_intlObject.isObject()) return;

    Rooted<Value> objProto{rtObjectPrototype()};
    Shape* dtfProtoShape = rtNewRootShape(objProto.get());
    dtfProtoShape->used_as_prototype = true;
    ObjectHeader* dtfProtoObj = ObjectHeader::create(rtHeap(), rtArena(), dtfProtoShape);
    dtfProtoObj->header.flags = HeapKind::Plain;
    Rooted<Value> proto{Value::fromObject(dtfProtoObj)};
    g_dtfProto = proto.get();
    rtHeap().add_permanent_root(&g_dtfProto);

    const NativeMethod kDtfProtoMethods[] = {
        {"format", dtfFormatMethod, 0, 1},
        {"formatToParts", dtfFormatToParts, 0, 1},
        {"resolvedOptions", dtfResolvedOptions, 0, 0},
    };
    rtDefineMethods(proto, kDtfProtoMethods, std::size(kDtfProtoMethods));
    rtDefineToStringTag(proto, "Intl.DateTimeFormat");

    Rooted<Value> ctor{rtNativeFunction(dtfConstructor, 0, "DateTimeFormat", 0)};
    rtEnsureFunctionProperties(ctor);
    {
        Rooted<Value> props{ctor.get().asObject<FunctionHeader>()->properties};
        Rooted<Value> slKey{rtMakeString("supportedLocalesOf")};
        Rooted<Value> slVal{rtNativeFunction(dtfSupportedLocalesOf, 0, "supportedLocalesOf", 1)};
        props.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), slKey, slVal, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
    }
    {
        Rooted<Value> ctorKey{rtMakeString("constructor")};
        proto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), ctorKey, ctor, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
    }

    g_dtfBrand = rtMakeSymbol(Value::fromUndefined()).asSymbol<SymbolHeader>();
    FunctionHeader* dtfFn = ctor.get().asObject<FunctionHeader>();
    dtfFn->prototype = proto.get();
    dtfFn->instance_shape = rtNewRootShape(proto.get());
    g_dtfInstanceShape = dtfFn->instance_shape;
    g_dtfCtor = ctor.get();
    rtHeap().add_permanent_root(&g_dtfCtor);

    Rooted<Value> intl{Value::fromObject(
        ObjectHeader::create(rtHeap(), rtArena(), rtNewRootShape(objProto.get())))};
    intl.get().asObject<ObjectHeader>()->header.flags = HeapKind::Plain;
    g_intlObject = intl.get();
    rtHeap().add_permanent_root(&g_intlObject);

    auto defineOnIntl = [&](const char* name, Rooted<Value>& val) {
        Rooted<Value> key{rtMakeString(name)};
        intl.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val, nullptr,
                                                     /*enumerable=*/false, /*defineOwn=*/true);
    };

    defineOnIntl("DateTimeFormat", ctor);

    Rooted<Value> nfCtor{rtNativeFunction(numberFormatCtor, 0, "NumberFormat", 0)};
    defineOnIntl("NumberFormat", nfCtor);

    Rooted<Value> colCtor{rtNativeFunction(collatorCtor, 0, "Collator", 0)};
    defineOnIntl("Collator", colCtor);

    Rooted<Value> gclFn{rtNativeFunction(getCanonicalLocales, 0, "getCanonicalLocales", 1)};
    defineOnIntl("getCanonicalLocales", gclFn);

    rtDefineToStringTag(intl, "Intl");
}

}  // namespace

Value rtIntlNamespace() {
    ensureIntlIntrinsics();
    return g_intlObject;
}

bool rtFormatDateLocale(double tv, uint32_t argc, const uint64_t* argv, bool defaultDate,
                        bool defaultTime, Value& out) {
    dt::DateTimeFormatOptions opts;
    if (!parseDateTimeFormatOptions(argc, argv, defaultDate, defaultTime,
                                    /*useDefaultZoneWhenOmitted=*/false, opts)) {
        return false;
    }
    if (std::isnan(tv)) {
        out = rtMakeString("Invalid Date");
        return true;
    }
    std::string formatted;
    if (!dt::formatDateTime(tv, opts, formatted)) {
        out = rtMakeString("Invalid Date");
        return true;
    }
    out = rtMakeString(formatted);
    return true;
}

}  // namespace bronze::runtime
