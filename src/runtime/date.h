#pragma once

#include <string>
#include <string_view>

// The PURE half of ECMA-262 §21.4: the calendar arithmetic every Date method is
// written in terms of, the local-time bridge, the pinned text formats, and the
// parser that reads them back. Nothing here touches the heap, allocates a JS
// value, or can raise — so every one of these is directly unit-testable
// (tests/runtime/date_math_test.cpp) without standing the runtime up.
//
// The JS surface — the constructor, the prototype object, the receiver checks —
// is builtin_date.cpp, and it is the only caller that knows what a Value is.
//
// Everything is a `double` holding milliseconds since the epoch, exactly as
// 21.4.1.1 defines a time value. That is not laziness about integers: NaN is a
// legal time value (an invalid Date) and has to propagate through every
// operation, and the range runs to ±8.64e15, which no 32-bit anything holds.
// An int64 shortcut would break the first and truncate the second.

namespace bronze::runtime::datetime {

inline constexpr double kMsPerSecond = 1000.0;
inline constexpr double kMsPerMinute = 60000.0;
inline constexpr double kMsPerHour = 3600000.0;
inline constexpr double kMsPerDay = 86400000.0;

// 21.4.1.1: the largest magnitude a time value may have — 100,000,000 days
// either side of the epoch. TimeClip turns anything beyond it into NaN.
inline constexpr double kMaxTimeValue = 8.64e15;

// 5.2.5 modulo: the result takes the sign of the DIVISOR, so `modulo(-1, 7)` is
// 6 and not -1. Every field extraction below leans on it, and it is the whole
// reason a pre-1970 date reports the right day of the week: C's `%` and a plain
// integer division truncate toward zero, which is off by one for every negative
// time value that is not an exact multiple.
double modulo(double x, double y);

// 21.4.1.2-21.4.1.12, in the specification's own names so a reader can check
// them against the clause without a translation step.
double day(double t);
double timeWithinDay(double t);
double daysInYear(double y);
double dayFromYear(double y);
double timeFromYear(double y);
double yearFromTime(double t);
bool inLeapYear(double t);
double dayWithinYear(double t);
double monthFromTime(double t);
double dateFromTime(double t);
double weekDay(double t);
double hourFromTime(double t);
double minFromTime(double t);
double secFromTime(double t);
double msFromTime(double t);

// 21.4.1.27-21.4.1.31. Each returns NaN when any input is non-finite, which is
// how an invalid Date stays invalid through a chain of them.
double makeTime(double hour, double min, double sec, double ms);
double makeDay(double year, double month, double date);
double makeDate(double day, double time);
double timeClip(double t);

// 21.4.1.32 MakeFullYear: the two-digit year mapping. 0-99 means 1900+y, and
// only after truncation — so `new Date(99.9, 0)` is 1999 and `new Date(100, 0)`
// is the year 100, not 2000.
double makeFullYear(double year);

// ---- the local-time bridge -------------------------------------------------
//
// 21.4.1.24 LocalTime / 21.4.1.25 UTC. The offset comes from the OS through the
// C library's `localtime`/`gmtime` pair, which reads the platform's zone
// database (Windows: the registry's dynamic DST rules) — so a DST transition is
// the OS's answer and not a rule hand-rolled here.
//
// One deliberate approximation, and it is here rather than in a document: the
// offset is queried at an instant CLAMPED into the window the C library
// accepts (1970-01-01 through roughly the year 3000). A date outside that
// window is given the offset in force at the nearest edge, so a program asking
// for the local fields of a Roman-era date gets today's rule for that zone
// rather than a crash or a garbage offset. Nothing in the range real programs
// use is affected, and the alternative — a full historic zone database — is not
// something bronze is going to carry.

// The milliseconds to ADD to a UTC time value to get the local one, at that
// instant. 0 for a non-finite input.
double localOffsetMs(double tUtc);
double localTime(double tUtc);

// The inverse. A local time value does not determine a UTC one uniquely across
// a DST transition, so this does what every engine does: guess with the offset
// read at the local instant, then re-read the offset at the guessed UTC instant
// and use that. 21.4.1.25 leaves the ambiguous hour implementation-defined.
double utcFromLocal(double tLocal);

// ---- the pinned text formats (date_text.cpp) --------------------------------

// 21.4.4.36 toISOString's bytes. False for a NaN time value, which is the
// RangeError the caller raises — the format has no spelling for one.
bool isoString(double t, std::string& out);

// 21.4.4.41 Date.prototype.toString, .35 toDateString, .42 toTimeString,
// .43 toUTCString. "Invalid Date" for NaN, which is what all four answer.
//
// DEVIATION, stated where it lives: 21.4.4.41.3 TimeZoneString ends with an
// implementation-defined zone NAME, and bronze emits none — the string stops
// after `GMT+HHMM`. Windows' zone names are localized and differ between
// machines, so printing one would make a program's output depend on the
// operator's display language. The offset is the part that carries meaning and
// it is fully determined by the zone.
std::string dateTimeString(double t);
std::string dateOnlyString(double t);
std::string timeOnlyString(double t);
std::string utcString(double t);

// What console.log prints for a Date: node's form, which is the ISO string
// bare (no quotes, no braces), and "Invalid Date" for NaN. Never allocates a JS
// value, because the inspect walk may not move the heap.
std::string inspectString(double t);

// ---- Date.parse (date_text.cpp) --------------------------------------------

// Why a parse produced no time value. `NotADate` is 21.4.3.2's answer — NaN.
// `RefusedFormat` is bronze's: the text is recognisably a date in a format node
// accepts and this parser does not, and answering NaN there would be a silent
// divergence from the engine every program was written against. The caller
// turns it into a hard error naming the format, which `refusedFormat` holds.
enum class ParseOutcome { Ok, NotADate, RefusedFormat };

// Accepts the 21.4.1.15 date-time string format (date-only, date-time, `Z`,
// `±HH:MM`, expanded ±YYYYYY years) and bronze's OWN `toString` and
// `toUTCString` output, so every string a Date prints round-trips.
//
// A date-only form with no offset is UTC; a date-time form with no offset is
// LOCAL. That split is 21.4.1.15's and is the one part of the grammar that
// surprises everybody.
ParseOutcome parse(std::string_view text, double& out, std::string& refusedFormat);

}  // namespace bronze::runtime::datetime
