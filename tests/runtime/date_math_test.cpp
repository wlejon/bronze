// The pure Date seam (src/runtime/date.h): §21.4.1's calendar arithmetic, the
// text formats, and Date.parse.
//
// This is where the off-by-ones live, and they are cheap to catch here and
// expensive to catch anywhere else: an oracle case proves a handful of dates,
// where a loop over ten thousand consecutive days proves the day-number
// arithmetic outright. Every assertion is timezone-independent — nothing below
// calls localTime, utcFromLocal or timeZoneString, because a machine's zone is
// not something a pinned test may depend on.

#include <doctest/doctest.h>

#include <cmath>
#include <string>

#include "runtime/date.h"

using namespace bronze::runtime::datetime;

namespace {

// A UTC time value from its fields, which is what every case below builds its
// input from.
double at(double y, double mo, double d, double h = 0, double mi = 0, double s = 0,
          double ms = 0) {
    return makeDate(makeDay(y, mo, d), makeTime(h, mi, s, ms));
}

}  // namespace

TEST_CASE("date modulo takes the sign of the divisor") {
    CHECK(modulo(-1.0, 7.0) == 6.0);
    CHECK(modulo(-7.0, 7.0) == 0.0);
    CHECK(modulo(8.0, 7.0) == 1.0);
    CHECK(modulo(-0.5, 1.0) == 0.5);
    // 5.2.5's zero is the mathematical one and therefore unsigned, where fmod's
    // keeps the dividend's sign. Every pre-epoch Sunday goes through this: the
    // weekday of 1969-07-20 is `modulo(-161, 7)`, and a -0 there prints as "-0".
    CHECK(!std::signbit(modulo(-161.0, 7.0)));
    CHECK(!std::signbit(modulo(-7.0, 7.0)));
    CHECK(!std::signbit(weekDay(-165.0 * kMsPerDay)));
    CHECK(weekDay(-165.0 * kMsPerDay) == 0.0);
}

TEST_CASE("date day numbering is exact at the epoch and its neighbours") {
    CHECK(dayFromYear(1970.0) == 0.0);
    CHECK(dayFromYear(1971.0) == 365.0);
    CHECK(dayFromYear(1969.0) == -365.0);
    CHECK(dayFromYear(2000.0) == 10957.0);
    CHECK(dayFromYear(1900.0) == -25567.0);
    // The epoch was a Thursday; the day before it a Wednesday.
    CHECK(weekDay(0.0) == 4.0);
    CHECK(weekDay(-1.0) == 3.0);
}

TEST_CASE("date leap years follow the Gregorian rule") {
    CHECK(daysInYear(1900.0) == 365.0);
    CHECK(daysInYear(2000.0) == 366.0);
    CHECK(daysInYear(2020.0) == 365.0 + 1.0);
    CHECK(daysInYear(2021.0) == 365.0);
    CHECK(daysInYear(2100.0) == 365.0);
}

TEST_CASE("date field extraction round-trips over ten thousand consecutive days") {
    // Straddles the epoch, so half of these are negative time values — which is
    // the half a truncating division gets wrong.
    double expectedYear = 1955.0;
    double expectedMonth = 0.0;
    double expectedDay = 1.0;
    double t = at(1955.0, 0.0, 1.0);
    for (int i = 0; i < 10000; ++i) {
        REQUIRE(yearFromTime(t) == expectedYear);
        REQUIRE(monthFromTime(t) == expectedMonth);
        REQUIRE(dateFromTime(t) == expectedDay);
        // MakeDay must agree with the extraction it is the inverse of.
        REQUIRE(makeDay(expectedYear, expectedMonth, expectedDay) == day(t));

        t += kMsPerDay;
        expectedDay += 1.0;
        static const double kLengths[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        double length = kLengths[static_cast<int>(expectedMonth)];
        if (expectedMonth == 1.0 && daysInYear(expectedYear) == 366.0) length = 29.0;
        if (expectedDay > length) {
            expectedDay = 1.0;
            expectedMonth += 1.0;
            if (expectedMonth == 12.0) {
                expectedMonth = 0.0;
                expectedYear += 1.0;
            }
        }
    }
}

TEST_CASE("date weekday advances by one per day in both directions") {
    for (int i = -5000; i < 5000; ++i) {
        const double t = static_cast<double>(i) * kMsPerDay;
        CHECK(weekDay(t) == modulo(static_cast<double>(i) + 4.0, 7.0));
    }
}

TEST_CASE("date pre-1970 fields floor rather than truncate") {
    // 1969-12-31T23:59:59.500Z — every field of it is a negative time value
    // divided by a positive modulus, which is the trap.
    const double t = -500.0;
    CHECK(yearFromTime(t) == 1969.0);
    CHECK(monthFromTime(t) == 11.0);
    CHECK(dateFromTime(t) == 31.0);
    CHECK(hourFromTime(t) == 23.0);
    CHECK(minFromTime(t) == 59.0);
    CHECK(secFromTime(t) == 59.0);
    CHECK(msFromTime(t) == 500.0);
    CHECK(day(t) == -1.0);
    CHECK(weekDay(t) == 3.0);
}

TEST_CASE("date MakeDay normalises month overflow in both directions") {
    // Month 12 of 2020 is January 2021; month -1 is December 2019.
    CHECK(makeDay(2020.0, 12.0, 1.0) == makeDay(2021.0, 0.0, 1.0));
    CHECK(makeDay(2020.0, -1.0, 1.0) == makeDay(2019.0, 11.0, 1.0));
    CHECK(makeDay(2020.0, -13.0, 1.0) == makeDay(2018.0, 11.0, 1.0));
    // Day overflow is not MakeDay's business to clamp: it adds `date - 1` days.
    CHECK(makeDay(2020.0, 0.0, 32.0) == makeDay(2020.0, 1.0, 1.0));
    CHECK(makeDay(2020.0, 0.0, 0.0) == makeDay(2019.0, 11.0, 31.0));
}

TEST_CASE("date MakeTime and MakeDate propagate NaN rather than guessing") {
    CHECK(std::isnan(makeTime(std::nan(""), 0, 0, 0)));
    CHECK(std::isnan(makeTime(0, 0, 0, INFINITY)));
    CHECK(std::isnan(makeDay(std::nan(""), 0, 0)));
    CHECK(std::isnan(makeDate(std::nan(""), 0)));
    CHECK(std::isnan(makeDate(0, std::nan(""))));
    // Truncation toward zero, per ToIntegerOrInfinity.
    CHECK(makeTime(1.9, 0, 0, 0) == kMsPerHour);
    CHECK(makeTime(-1.9, 0, 0, 0) == -kMsPerHour);
}

TEST_CASE("date TimeClip refuses beyond 8.64e15 and folds negative zero") {
    CHECK(timeClip(kMaxTimeValue) == kMaxTimeValue);
    CHECK(std::isnan(timeClip(kMaxTimeValue + 1.0)));
    CHECK(std::isnan(timeClip(-kMaxTimeValue - 1.0)));
    CHECK(std::isnan(timeClip(std::nan(""))));
    CHECK(std::isnan(timeClip(INFINITY)));
    CHECK(timeClip(-0.5) == 0.0);
    CHECK(!std::signbit(timeClip(-0.5)));
    CHECK(timeClip(1.9) == 1.0);
    CHECK(timeClip(-1.9) == -1.0);
}

TEST_CASE("date MakeFullYear maps only the truncated range 0 to 99") {
    CHECK(makeFullYear(0.0) == 1900.0);
    CHECK(makeFullYear(99.0) == 1999.0);
    CHECK(makeFullYear(99.9) == 1999.0);
    CHECK(makeFullYear(100.0) == 100.0);
    CHECK(makeFullYear(-1.0) == -1.0);
    CHECK(makeFullYear(2020.0) == 2020.0);
    CHECK(std::isnan(makeFullYear(std::nan(""))));
}

TEST_CASE("date ISO strings use the expanded year form outside 0000-9999") {
    std::string out;
    REQUIRE(isoString(0.0, out));
    CHECK(out == "1970-01-01T00:00:00.000Z");
    REQUIRE(isoString(at(2020.0, 0.0, 1.0, 12.0, 34.0, 56.0, 789.0), out));
    CHECK(out == "2020-01-01T12:34:56.789Z");
    REQUIRE(isoString(at(-1.0, 0.0, 1.0), out));
    CHECK(out == "-000001-01-01T00:00:00.000Z");
    REQUIRE(isoString(at(10000.0, 0.0, 1.0), out));
    CHECK(out == "+010000-01-01T00:00:00.000Z");
    REQUIRE(isoString(kMaxTimeValue, out));
    CHECK(out == "+275760-09-13T00:00:00.000Z");
    REQUIRE(isoString(-kMaxTimeValue, out));
    CHECK(out == "-271821-04-20T00:00:00.000Z");
    CHECK(!isoString(std::nan(""), out));
}

TEST_CASE("date toUTCString matches 21.4.4.43's grammar") {
    CHECK(utcString(0.0) == "Thu, 01 Jan 1970 00:00:00 GMT");
    CHECK(utcString(at(2020.0, 1.0, 29.0, 13.0, 5.0, 9.0)) ==
          "Sat, 29 Feb 2020 13:05:09 GMT");
    CHECK(utcString(std::nan("")) == "Invalid Date");
}

TEST_CASE("date inspect prints the ISO form and names an invalid date") {
    CHECK(inspectString(0.0) == "1970-01-01T00:00:00.000Z");
    CHECK(inspectString(std::nan("")) == "Invalid Date");
}

// Every input below either carries its zone (Z, GMT, an offset) or is NaN, so
// that the expected value is the same on every machine. The local-time forms
// are covered by the oracle case tests/oracle/cases/date_parse_legacy.js
// through relations that cancel the zone out.

TEST_CASE("date parse reads the ISO date-time format") {
    CHECK(parse("1970-01-01T00:00:00.000Z") == 0.0);
    CHECK(parse("2020-01-01T00:00:00Z") == at(2020.0, 0.0, 1.0));
    CHECK(parse("2020-01-01T00:00Z") == at(2020.0, 0.0, 1.0));
    // A date-only form is UTC with no offset written; that is 21.4.1.15's rule
    // and the one part of the grammar that surprises everybody.
    CHECK(parse("2020-01-01") == at(2020.0, 0.0, 1.0));
    CHECK(parse("2020-01") == at(2020.0, 0.0, 1.0));
    CHECK(parse("2020") == at(2020.0, 0.0, 1.0));
    // Explicit offsets, in both directions and in both spellings.
    CHECK(parse("2020-01-01T00:00:00+05:30") == at(2020.0, 0.0, 1.0) - 5.5 * kMsPerHour);
    CHECK(parse("2020-01-01T00:00:00-05:00") == at(2020.0, 0.0, 1.0) + 5.0 * kMsPerHour);
    CHECK(parse("2020-01-01T00:00:00+0530") == at(2020.0, 0.0, 1.0) - 5.5 * kMsPerHour);
    // The expanded year form, both signs.
    CHECK(parse("+010000-01-01T00:00:00Z") == at(10000.0, 0.0, 1.0));
    CHECK(parse("-000001-01-01T00:00:00Z") == at(-1.0, 0.0, 1.0));
    CHECK(parse("+275760-09-13T00:00:00.000Z") == kMaxTimeValue);
    // 24:00 is the end of a day and only with zero minutes, seconds and ms.
    CHECK(parse("2020-01-01T24:00:00Z") == at(2020.0, 0.0, 2.0));
    // A fraction longer than three digits truncates rather than rounding.
    CHECK(parse("2020-01-01T00:00:00.9999Z") == at(2020.0, 0.0, 1.0, 0, 0, 0, 999.0));
    CHECK(parse("2020-01-01T00:00:00.1Z") == at(2020.0, 0.0, 1.0, 0, 0, 0, 100.0));
    // Out of range clips to NaN rather than wrapping.
    CHECK(std::isnan(parse("+275760-09-14T00:00:00Z")));
}

TEST_CASE("date parse rejects out-of-bounds fields, and only those") {
    for (const char* bad : {"2020-13-01", "2020-00-01", "2020-01-32", "2020-01-00",
                            "2020-01-01T25:00:00Z", "2020-01-01T24:00:01Z",
                            "2020-01-01T00:60:00Z", "2020-01-01T00:00:60Z", "-000000",
                            "2020-01-01T0:00Z", "2020-01-01T10:00Zx", "2020-01-01Tgarbage",
                            "2020-01-01+01:00"}) {
        CAPTURE(bad);
        CHECK(std::isnan(parse(bad)));
    }
    // The day is checked against 31, never against the month: a date the
    // calendar does not have is carried forward by MakeDay, which is what
    // every engine does and what `new Date(2020, 1, 30)` does.
    CHECK(parse("2020-02-30") == at(2020.0, 2.0, 1.0));
    CHECK(parse("2019-02-29") == at(2019.0, 2.0, 1.0));
    CHECK(parse("2020-02-29") == at(2020.0, 1.0, 29.0));
}

TEST_CASE("date parse round-trips bronze's own toString and toUTCString") {
    for (double tv : {0.0, 1.0, -1.0, at(2020.0, 1.0, 29.0, 13.0, 5.0, 9.0),
                      at(1950.0, 6.0, 4.0, 1.0, 2.0, 3.0)}) {
        // toUTCString drops the milliseconds, so the round-trip is exact only
        // to the second — which is what makes this the right comparison.
        CHECK(parse(utcString(tv)) == tv - modulo(tv, kMsPerSecond));
        CHECK(parse(dateTimeString(tv)) == tv - modulo(tv, kMsPerSecond));
    }
}

TEST_CASE("date parse answers NaN for text that is not a date at all") {
    for (const char* junk : {"", "   ", "hello", "mayonnaise", "2020-01-01X",
                             "not a date at all", "Sunshine", ":", "-", "+", ")", "()",
                             "1x Jan 2020", "Jan 1x 2020", "Jan 1 2020 foo",
                             "Jan 1 2020 10:00 GMT+0100 extra", "Jan 1 2020 5pm"}) {
        CAPTURE(junk);
        CHECK(std::isnan(parse(junk)));
    }
}

TEST_CASE("date parse reads the legacy forms with an explicit zone as V8 does") {
    const double jan1 = at(2020.0, 0.0, 1.0);
    const double ten = jan1 + 10.0 * kMsPerHour;
    CHECK(parse("Jan 1 2020 10:00 GMT") == ten);
    CHECK(parse("Jan 1 2020 10:00 UTC") == ten);
    CHECK(parse("Jan 1 2020 10:00 UT") == ten);
    CHECK(parse("Jan 1 2020 10:00 Z") == ten);
    CHECK(parse("January 1, 2020 10:00 GMT") == ten);
    CHECK(parse("1 January 2020 10:00 GMT") == ten);
    CHECK(parse("2020 Jan 1 10:00 GMT") == ten);
    CHECK(parse("Wed Jan 01 2020 10:00:00 GMT+0000 (Coordinated Universal Time)") == ten);
    CHECK(parse("Wed, 01 Jan 2020 10:00:00 +0000") == ten);
    CHECK(parse("Wed, 01 Jan 2020 10:00:00 GMT") == ten);
    CHECK(parse("1/1/2020 10:00:00 AM UTC") == ten);
    CHECK(parse("1/1/2020 10:00:00 PM UTC") == ten + 12.0 * kMsPerHour);
    CHECK(parse("2020/01/01 10:00 GMT") == ten);
    CHECK(parse("2020-01-01 10:00:00Z") == ten);
    CHECK(parse("2020-01-01 10:00 GMT+0100") == ten - kMsPerHour);
    // Offsets in every spelling V8 takes after a time or after UTC.
    CHECK(parse("Jan 1 2020 10:00 GMT+1") == ten - kMsPerHour);
    CHECK(parse("Jan 1 2020 10:00 GMT+01:30") == ten - 1.5 * kMsPerHour);
    CHECK(parse("Jan 1 2020 10:00 +0130") == ten - 1.5 * kMsPerHour);
    CHECK(parse("Jan 1 2020 10:00 -05") == ten + 5.0 * kMsPerHour);
    CHECK(parse("Jan 1 2020 10:00-05:00") == ten + 5.0 * kMsPerHour);
    CHECK(parse("Jan 1 2020 10:00 -500") == ten + 5.0 * kMsPerHour);
    CHECK(parse("Jan 1 2020 10:00 EST") == ten + 5.0 * kMsPerHour);
    CHECK(parse("Jan 1 2020 10:00 PDT") == ten + 7.0 * kMsPerHour);
    CHECK(parse("Jan 1 2020 GMT-5") == jan1 + 5.0 * kMsPerHour);
    // The named zones are 3-letter prefixes; "CEST" is a garbage word.
    CHECK(std::isnan(parse("Jan 1 2020 10:00 CEST")));
    // am/pm only attaches to a time already read, and needs an hour 0..12.
    CHECK(parse("Jan 1 2020 12:00 am GMT") == jan1);
    CHECK(parse("Jan 1 2020 12:00 pm GMT") == jan1 + 12.0 * kMsPerHour);
    CHECK(std::isnan(parse("Jan 1 2020 13:00 pm GMT")));
    // The KJS-era year rules: a bare component that is not a day is the year,
    // 0-49 map to 2000-2049 and 50-99 to 1950-1999, and a missing day fills
    // as 1 — so "Jan 1 GMT" is January 1st of the year 2001.
    CHECK(parse("Jan 1 20 GMT") == at(2020.0, 0.0, 1.0));
    CHECK(parse("Jan 1 99 GMT") == at(1999.0, 0.0, 1.0));
    CHECK(parse("Jan 1 100 GMT") == at(100.0, 0.0, 1.0));
    CHECK(parse("Jan 1 GMT") == at(2001.0, 0.0, 1.0));
    CHECK(parse("0 GMT") == at(2000.0, 0.0, 1.0));
    // Feb 31 is a legal day component and carries into March.
    CHECK(parse("Feb 31 2020 GMT") == at(2020.0, 2.0, 2.0));
    // Text in parentheses is skipped wherever it falls.
    CHECK(parse("Jan 1 2020 (a (nested) paren) 10:00 GMT") == ten);
    CHECK(parse("(x) Jan 1 2020 10:00 GMT") == ten);
    // Milliseconds after a time keep their first three significant digits.
    CHECK(parse("Jan 1 2020 10:00:00.5 GMT") == ten + 500.0);
    CHECK(parse("Jan 1 2020 10:00:00.123456 GMT") == ten + 123.0);
    CHECK(parse("Jan 1 2020 10:00:00.05 GMT") == ten + 50.0);
}

TEST_CASE("date parse treats surrounding whitespace as V8 does") {
    // Whitespace is a token, not something stripped first. A leading space
    // takes the string out of the ES5 grammar, so the `T` is then a garbage
    // word after a number and the parse fails; a date-only string with a
    // space around it is read by the legacy loop instead, which agrees with
    // the ES5 reading only once a zone is written (without one it is local,
    // which a zone-independent test cannot pin).
    CHECK(std::isnan(parse(" 2020-01-01T00:00:00Z")));
    CHECK(std::isnan(parse("2020-01-01T00:00:00Z ")));
    CHECK(parse(" 2020-01-01 GMT ") == at(2020.0, 0.0, 1.0));
    CHECK(parse("2020- 01-01 GMT") == at(2020.0, 0.0, 1.0));
}
