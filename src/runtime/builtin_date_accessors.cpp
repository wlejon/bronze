// `Date.prototype`'s field getters and setters (ECMA-262 21.4.4.2 - 21.4.4.31).
//
// Thirty-three members and four algorithms. Each family is written ONCE as a
// template and instantiated per member, which is how `builtin_data_view.cpp`
// gives its sixteen accessors distinct code pointers — and the code pointer is
// what `rtNativeFunction` interns on, so two members sharing an instantiation
// would be one function object with two names.
//
// The two halves of every family — local and UTC — differ by exactly the pair
// of conversions at the edges, so `Local` is a template parameter rather than a
// second copy of each body. That is not tidiness: `getHours` and `getUTCHours`
// disagreeing about anything but the timezone would be a bug no test that pins
// only one of them could see.

#include <cmath>
#include <cstddef>
#include <iterator>

#include "runtime/builtin_date_internal.h"
#include "runtime/date.h"
#include "runtime/exception.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

namespace dt = datetime;

// The conversions the local/UTC split turns on. `UTC(LocalTime(t))` is not
// quite the identity across a DST seam, which is why both directions are named
// rather than one being derived from the other.
double toZone(double t, bool local) { return local ? dt::localTime(t) : t; }
double fromZone(double t, bool local) { return local ? dt::utcFromLocal(t) : t; }

// ---- the getters ------------------------------------------------------------

using FieldOf = double (*)(double);

// 21.4.4.2 - 21.4.4.19 in one body: read [[DateValue]], propagate NaN, move into
// the requested zone, extract. Sixteen members, sixteen instantiations, and no
// two of them share an (extractor, zone) pair — which they must not, since a
// shared pair would be a shared code pointer and therefore one function object
// answering to two names.
template <FieldOf Extract, bool Local>
uint64_t dateGetField(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    double t = 0.0;
    if (!rtDateThisTimeValue(Value(thisBits), "a Date getter", t)) {
        return Value::fromUndefined().rawBits();
    }
    if (std::isnan(t)) return Value::fromDouble(t).rawBits();
    return Value::fromDouble(Extract(toZone(t, Local))).rawBits();
}

// 21.4.4.10 getTime and 21.4.4.44 valueOf are the same answer and two function
// objects, because 21.4.4 gives them two names. Written twice rather than
// aliased for exactly that reason.
uint64_t dateGetTime(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    double t = 0.0;
    if (!rtDateThisTimeValue(Value(thisBits), "getTime", t)) {
        return Value::fromUndefined().rawBits();
    }
    return Value::fromDouble(t).rawBits();
}

uint64_t dateValueOf(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    double t = 0.0;
    if (!rtDateThisTimeValue(Value(thisBits), "valueOf", t)) {
        return Value::fromUndefined().rawBits();
    }
    return Value::fromDouble(t).rawBits();
}

// 21.4.4.9. The sign is the one thing about this member everybody gets wrong:
// it reports how far the local zone is BEHIND UTC, so a zone ahead of UTC has a
// negative offset.
uint64_t dateGetTimezoneOffset(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    double t = 0.0;
    if (!rtDateThisTimeValue(Value(thisBits), "getTimezoneOffset", t)) {
        return Value::fromUndefined().rawBits();
    }
    if (std::isnan(t)) return Value::fromDouble(t).rawBits();
    return Value::fromDouble((t - dt::localTime(t)) / dt::kMsPerMinute).rawBits();
}

// ---- the setters ------------------------------------------------------------

// 21.4.4.27 setTime, which is the only setter that does not read the old value.
uint64_t dateSetTime(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    double ignored = 0.0;
    if (!rtDateThisTimeValue(self.get(), "setTime", ignored)) {
        return Value::fromUndefined().rawBits();
    }
    const double t = dt::timeClip(rtToNumber(args[0]));
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    rtDateSetTimeValue(self.get(), t);
    return Value::fromDouble(t).rawBits();
}

// 21.4.4.20 setHours / .23 setMilliseconds / .24 setMinutes / .26 setSeconds and
// their UTC twins, which are one algorithm with a different number of leading
// fields supplied. `First` names the leading field: 0 hours, 1 minutes,
// 2 seconds, 3 milliseconds.
//
// The ORDER matters and is the clause's: every argument is converted with
// ToNumber BEFORE the NaN check on the receiver, so a setter called on an
// invalid Date still runs its arguments' `valueOf`. Only after that does an
// invalid Date short-circuit to NaN.
template <int First, bool Local>
uint64_t dateSetTimeFields(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    double t = 0.0;
    if (!rtDateThisTimeValue(self.get(), "a Date setter", t)) {
        return Value::fromUndefined().rawBits();
    }
    constexpr int kSettable = 4 - First;
    double supplied[4] = {0.0, 0.0, 0.0, 0.0};
    // The first field is always taken from the arguments, even when the call
    // passed none: `d.setHours()` is `d.setHours(undefined)`, ToNumber of that
    // is NaN, and the whole date becomes invalid. Only the TRAILING fields are
    // conditional, and "present" means the call really passed them.
    supplied[First] = rtToNumber(args[0]);
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    int given = 1;
    for (int i = 1; i < kSettable; ++i) {
        if (static_cast<uint32_t>(i) >= args.count()) break;
        supplied[First + i] = rtToNumber(args[static_cast<uint32_t>(i)]);
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        ++given;
    }
    if (std::isnan(t)) return Value::fromDouble(t).rawBits();

    const double zoned = toZone(t, Local);
    double fields[4] = {dt::hourFromTime(zoned), dt::minFromTime(zoned), dt::secFromTime(zoned),
                        dt::msFromTime(zoned)};
    for (int i = 0; i < given; ++i) fields[First + i] = supplied[First + i];

    const double when = dt::makeDate(dt::day(zoned),
                                     dt::makeTime(fields[0], fields[1], fields[2], fields[3]));
    const double u = dt::timeClip(fromZone(when, Local));
    rtDateSetTimeValue(self.get(), u);
    return Value::fromDouble(u).rawBits();
}

// 21.4.4.21 setFullYear / .22 setMonth / .11 setDate and their UTC twins.
// `First` names the leading field: 0 year, 1 month, 2 date.
//
// The year form has a rule of its own (21.4.4.21 step 3) and it is the reason
// this is not merged with the block above: setting the YEAR of an invalid Date
// works, starting from the epoch, where every other setter answers NaN. So
// `new Date(NaN).setFullYear(2020)` is a real date and
// `new Date(NaN).setMonth(0)` is still NaN.
template <int First, bool Local>
uint64_t dateSetDateFields(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    double t = 0.0;
    if (!rtDateThisTimeValue(self.get(), "a Date setter", t)) {
        return Value::fromUndefined().rawBits();
    }
    constexpr int kSettable = 3 - First;
    double supplied[3] = {0.0, 0.0, 0.0};
    supplied[First] = rtToNumber(args[0]);
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    int given = 1;
    for (int i = 1; i < kSettable; ++i) {
        if (static_cast<uint32_t>(i) >= args.count()) break;
        supplied[First + i] = rtToNumber(args[static_cast<uint32_t>(i)]);
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        ++given;
    }

    double zoned = 0.0;
    if (std::isnan(t)) {
        if constexpr (First != 0) {
            return Value::fromDouble(t).rawBits();
        } else {
            // Step 3: an invalid Date is treated as the epoch, in LOCAL terms —
            // the clause sets t to +0 and does NOT run LocalTime over it.
            zoned = 0.0;
        }
    } else {
        zoned = toZone(t, Local);
    }

    double fields[3] = {dt::yearFromTime(zoned), dt::monthFromTime(zoned),
                        dt::dateFromTime(zoned)};
    for (int i = 0; i < given; ++i) fields[First + i] = supplied[First + i];

    const double when = dt::makeDate(dt::makeDay(fields[0], fields[1], fields[2]),
                                     dt::timeWithinDay(zoned));
    const double u = dt::timeClip(fromZone(when, Local));
    rtDateSetTimeValue(self.get(), u);
    return Value::fromDouble(u).rawBits();
}

// Every arity is 0, and that is load-bearing rather than lazy. `arity` in a
// NativeMethod is the count a SHORT CALL IS PADDED TO (rt_builtins.h), and half
// of 21.4.4's setters branch on whether a trailing argument was PRESENT — so
// padding `d.setHours(1)` out to four `undefined`s would set the minutes,
// seconds and milliseconds to NaN and invalidate the date. Nothing here reads
// past `args.count()` anyway: RootedArgs answers `undefined` for an index the
// call did not supply, which is exactly what the algorithms want.
const NativeMethod kAccessors[] = {
    {"getTime", dateGetTime, 0},
    {"valueOf", dateValueOf, 0},
    {"getTimezoneOffset", dateGetTimezoneOffset, 0},

    {"getFullYear", dateGetField<dt::yearFromTime, true>, 0},
    {"getMonth", dateGetField<dt::monthFromTime, true>, 0},
    {"getDate", dateGetField<dt::dateFromTime, true>, 0},
    {"getDay", dateGetField<dt::weekDay, true>, 0},
    {"getHours", dateGetField<dt::hourFromTime, true>, 0},
    {"getMinutes", dateGetField<dt::minFromTime, true>, 0},
    {"getSeconds", dateGetField<dt::secFromTime, true>, 0},
    {"getMilliseconds", dateGetField<dt::msFromTime, true>, 0},

    {"getUTCFullYear", dateGetField<dt::yearFromTime, false>, 0},
    {"getUTCMonth", dateGetField<dt::monthFromTime, false>, 0},
    {"getUTCDate", dateGetField<dt::dateFromTime, false>, 0},
    {"getUTCDay", dateGetField<dt::weekDay, false>, 0},
    {"getUTCHours", dateGetField<dt::hourFromTime, false>, 0},
    {"getUTCMinutes", dateGetField<dt::minFromTime, false>, 0},
    {"getUTCSeconds", dateGetField<dt::secFromTime, false>, 0},
    {"getUTCMilliseconds", dateGetField<dt::msFromTime, false>, 0},

    {"setTime", dateSetTime, 0},
    {"setHours", dateSetTimeFields<0, true>, 0},
    {"setMinutes", dateSetTimeFields<1, true>, 0},
    {"setSeconds", dateSetTimeFields<2, true>, 0},
    {"setMilliseconds", dateSetTimeFields<3, true>, 0},
    {"setUTCHours", dateSetTimeFields<0, false>, 0},
    {"setUTCMinutes", dateSetTimeFields<1, false>, 0},
    {"setUTCSeconds", dateSetTimeFields<2, false>, 0},
    {"setUTCMilliseconds", dateSetTimeFields<3, false>, 0},

    {"setFullYear", dateSetDateFields<0, true>, 0},
    {"setMonth", dateSetDateFields<1, true>, 0},
    {"setDate", dateSetDateFields<2, true>, 0},
    {"setUTCFullYear", dateSetDateFields<0, false>, 0},
    {"setUTCMonth", dateSetDateFields<1, false>, 0},
    {"setUTCDate", dateSetDateFields<2, false>, 0},
};

}  // namespace

const NativeMethod* rtDateAccessorMethods(size_t& count) {
    count = std::size(kAccessors);
    return kAccessors;
}

}  // namespace bronze::runtime
