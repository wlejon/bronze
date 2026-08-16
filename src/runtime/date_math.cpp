// ECMA-262 §21.4.1's abstract operations: the day-number arithmetic, the field
// extractions, MakeTime/MakeDay/MakeDate/TimeClip, and the bridge to the
// operating system's timezone.
//
// Every function is total over the doubles. NaN in, NaN out, without a branch
// that would turn an invalid Date into a plausible-looking one.

#include "runtime/date.h"

#include <cmath>
#include <ctime>

namespace bronze::runtime::datetime {

namespace {

// Days from the start of the year to the start of each month, in a non-leap
// year, with a thirteenth entry so a month's END is a lookup rather than a
// special case for December.
constexpr double kMonthStart[13] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365};

bool isLeapYear(double y) {
    if (modulo(y, 4.0) != 0.0) return false;
    if (modulo(y, 100.0) != 0.0) return true;
    return modulo(y, 400.0) == 0.0;
}

// The day of the year month `m` (0-based) begins on.
double monthStartDay(double m, bool leap) {
    const double base = kMonthStart[static_cast<int>(m)];
    return (m >= 2.0 && leap) ? base + 1.0 : base;
}

}  // namespace

double modulo(double x, double y) {
    const double r = std::fmod(x, y);
    // 5.2.5 is a MATHEMATICAL value, and its zero is unsigned. fmod's is not:
    // an exact division keeps the dividend's sign, so `fmod(-161, 7)` is -0 —
    // which came back out of `getUTCDay` as "-0" for every Sunday before the
    // epoch. Folded first, because the sign test below cannot tell -0 from +0.
    if (r == 0.0) return 0.0;
    // fmod takes the sign of the DIVIDEND; 5.2.5 takes the sign of the divisor.
    if ((r < 0.0) != (y < 0.0)) return r + y;
    return r;
}

double day(double t) { return std::floor(t / kMsPerDay); }

double timeWithinDay(double t) { return modulo(t, kMsPerDay); }

double daysInYear(double y) { return isLeapYear(y) ? 366.0 : 365.0; }

// 21.4.1.4. The three correction terms are the Gregorian leap rule written as
// day counts: every fourth year, minus every hundredth, plus every four
// hundredth, each anchored at the last such year at or before 1970.
double dayFromYear(double y) {
    return 365.0 * (y - 1970.0) + std::floor((y - 1969.0) / 4.0) -
           std::floor((y - 1901.0) / 100.0) + std::floor((y - 1601.0) / 400.0);
}

double timeFromYear(double y) { return kMsPerDay * dayFromYear(y); }

// 21.4.1.3: the largest y with TimeFromYear(y) <= t. The estimate uses the mean
// Gregorian year (365.2425 days) and is never more than one year out, so the
// two corrections below run at most once each — but they are `while` loops
// rather than single steps because "never more than one" is an assertion about
// the estimate, and a loop costs nothing to make it true by construction.
double yearFromTime(double t) {
    if (!std::isfinite(t)) return std::nan("");
    double y = std::floor(t / (kMsPerDay * 365.2425)) + 1970.0;
    while (timeFromYear(y) > t) y -= 1.0;
    while (timeFromYear(y + 1.0) <= t) y += 1.0;
    return y;
}

bool inLeapYear(double t) { return isLeapYear(yearFromTime(t)); }

double dayWithinYear(double t) { return day(t) - dayFromYear(yearFromTime(t)); }

double monthFromTime(double t) {
    if (!std::isfinite(t)) return std::nan("");
    const double d = dayWithinYear(t);
    const bool leap = inLeapYear(t);
    for (double m = 11.0; m >= 0.0; m -= 1.0) {
        if (d >= monthStartDay(m, leap)) return m;
    }
    return 0.0;
}

double dateFromTime(double t) {
    if (!std::isfinite(t)) return std::nan("");
    const double m = monthFromTime(t);
    return dayWithinYear(t) - monthStartDay(m, inLeapYear(t)) + 1.0;
}

// 21.4.1.7. The epoch was a Thursday, which is the `+ 4`.
double weekDay(double t) {
    if (!std::isfinite(t)) return std::nan("");
    return modulo(day(t) + 4.0, 7.0);
}

double hourFromTime(double t) { return modulo(std::floor(t / kMsPerHour), 24.0); }
double minFromTime(double t) { return modulo(std::floor(t / kMsPerMinute), 60.0); }
double secFromTime(double t) { return modulo(std::floor(t / kMsPerSecond), 60.0); }
double msFromTime(double t) { return modulo(t, kMsPerSecond); }

double makeTime(double hour, double min, double sec, double ms) {
    if (!std::isfinite(hour) || !std::isfinite(min) || !std::isfinite(sec) ||
        !std::isfinite(ms)) {
        return std::nan("");
    }
    // No range check: 21.4.1.27 does not clamp, which is exactly what makes
    // `new Date(2020, 0, 1, 25)` normalise into the next day.
    return std::trunc(hour) * kMsPerHour + std::trunc(min) * kMsPerMinute +
           std::trunc(sec) * kMsPerSecond + std::trunc(ms);
}

double makeDay(double year, double month, double date) {
    if (!std::isfinite(year) || !std::isfinite(month) || !std::isfinite(date)) {
        return std::nan("");
    }
    const double y = std::trunc(year);
    const double m = std::trunc(month);
    const double dt = std::trunc(date);
    // Month overflow rolls the YEAR, in both directions: month 12 is January of
    // the next year and month -1 is December of the previous one. `floor` is
    // what makes the negative half work, where a truncating division would send
    // month -1 to year y and month 11.
    const double ym = y + std::floor(m / 12.0);
    if (!std::isfinite(ym)) return std::nan("");
    const double mn = modulo(m, 12.0);
    // 21.4.1.28 step 8: "if no such t exists, return NaN". The representable
    // years run to ±275760; anything past this bound cannot produce a time
    // value TimeClip would keep, and stopping here also keeps `dayFromYear`
    // away from magnitudes where a double stops counting whole days.
    if (std::abs(ym) > 400000.0) return std::nan("");
    return dayFromYear(ym) + monthStartDay(mn, isLeapYear(ym)) + dt - 1.0;
}

double makeDate(double day, double time) {
    if (!std::isfinite(day) || !std::isfinite(time)) return std::nan("");
    const double tv = day * kMsPerDay + time;
    if (!std::isfinite(tv)) return std::nan("");
    return tv;
}

double timeClip(double t) {
    if (!std::isfinite(t)) return std::nan("");
    if (std::abs(t) > kMaxTimeValue) return std::nan("");
    const double clipped = std::trunc(t);
    // 21.4.1.31 step 4 folds -0 into +0, so that `new Date(-0.5).getTime()` is
    // 0 and prints "0" rather than "-0".
    return clipped == 0.0 ? 0.0 : clipped;
}

double makeFullYear(double year) {
    if (std::isnan(year)) return std::nan("");
    const double truncated = std::trunc(year);
    if (truncated >= 0.0 && truncated <= 99.0) return 1900.0 + truncated;
    return truncated;
}

// ---- the OS timezone bridge -------------------------------------------------

namespace {

// The window `localtime` accepts on every platform bronze builds for: the epoch
// through 3000-01-01. Outside it the C library either fails or answers
// nonsense, and the header says what the clamp costs.
constexpr double kMinQuerySeconds = 0.0;
constexpr double kMaxQuerySeconds = 32503680000.0;

// A broken-down time as a time value, using this file's own calendar math
// rather than `mktime`/`timegm`. `mktime` would re-apply the zone offset (the
// thing being measured) and `timegm` is not portable; the fields are already
// the answer, so all that is wanted is to weigh them.
double tmAsTimeValue(const std::tm& parts) {
    return makeDate(makeDay(static_cast<double>(parts.tm_year) + 1900.0,
                            static_cast<double>(parts.tm_mon),
                            static_cast<double>(parts.tm_mday)),
                    makeTime(static_cast<double>(parts.tm_hour),
                             static_cast<double>(parts.tm_min),
                             static_cast<double>(parts.tm_sec), 0.0));
}

}  // namespace

double localOffsetMs(double tUtc) {
    if (!std::isfinite(tUtc)) return 0.0;
    double secs = std::floor(tUtc / kMsPerSecond);
    if (secs < kMinQuerySeconds) secs = kMinQuerySeconds;
    if (secs > kMaxQuerySeconds) secs = kMaxQuerySeconds;
    const std::time_t stamp = static_cast<std::time_t>(secs);

    std::tm asLocal{};
    std::tm asUtc{};
#ifdef _WIN32
    // MSVC's localtime_s/gmtime_s take the output first — the opposite of C11's
    // annex K signature, and the reason this is not written once for both.
    if (localtime_s(&asLocal, &stamp) != 0) return 0.0;
    if (gmtime_s(&asUtc, &stamp) != 0) return 0.0;
#else
    if (localtime_r(&stamp, &asLocal) == nullptr) return 0.0;
    if (gmtime_r(&stamp, &asUtc) == nullptr) return 0.0;
#endif
    const double diff = tmAsTimeValue(asLocal) - tmAsTimeValue(asUtc);
    return std::isfinite(diff) ? diff : 0.0;
}

double localTime(double tUtc) {
    if (!std::isfinite(tUtc)) return tUtc;
    return tUtc + localOffsetMs(tUtc);
}

double utcFromLocal(double tLocal) {
    if (!std::isfinite(tLocal)) return tLocal;
    // Two passes. The first reads the offset as though the local wall time were
    // a UTC instant, which is wrong by at most the offset itself; the second
    // reads it at the instant that guess names, which is right except inside
    // the hour a DST shift skips or repeats — where 21.4.1.25 leaves the answer
    // to the implementation anyway.
    const double guess = tLocal - localOffsetMs(tLocal);
    return tLocal - localOffsetMs(guess);
}

}  // namespace bronze::runtime::datetime
