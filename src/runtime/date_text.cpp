// The Date TEXT formats: what §21.4.4's five string methods print, and what
// §21.4.3.2 `Date.parse` reads back.
//
// Formatting and parsing live in one file because they are one contract. Every
// string this half prints, the other half must accept — `Date.parse(d.toString())`
// and `Date.parse(d.toISOString())` are round-trips a program is entitled to —
// and splitting them is how a padding change on one side becomes a parse
// failure on the other.

#include "runtime/date.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>

namespace bronze::runtime::datetime {

namespace {

const char* const kWeekdayNames[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* const kMonthNames[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// The full English month names, for the REFUSAL recogniser alone. bronze never
// prints one and never parses one; the list exists so that `new Date("January
// 1, 2020")` — which node accepts — dies naming the format instead of quietly
// answering NaN.
const char* const kFullMonthNames[12] = {"january", "february", "march",     "april",
                                         "may",     "june",     "july",      "august",
                                         "september", "october", "november", "december"};

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// A non-negative integral double as decimal, left-padded with zeros to at
// least `width` characters. `std::to_string` on a double would print a
// fractional tail; every value that reaches here is integral by construction.
std::string pad(double value, int width) {
    unsigned long long n = static_cast<unsigned long long>(value);
    std::string digits;
    do {
        digits.push_back(static_cast<char>('0' + n % 10));
        n /= 10;
    } while (n != 0);
    while (digits.size() < static_cast<size_t>(width)) digits.push_back('0');
    return std::string(digits.rbegin(), digits.rend());
}

// 21.4.4.41.1 DateString's year field, and 21.4.4.43's: a negative year is
// written with a leading minus and the magnitude padded, never as a
// two's-complement-looking thing.
std::string yearField(double t) {
    const double y = yearFromTime(t);
    if (y < 0.0) return "-" + pad(-y, 4);
    return pad(y, 4);
}

// 21.4.4.41.1 DateString.
std::string dateString(double t) {
    std::string out = kWeekdayNames[static_cast<int>(weekDay(t))];
    out += ' ';
    out += kMonthNames[static_cast<int>(monthFromTime(t))];
    out += ' ';
    out += pad(dateFromTime(t), 2);
    out += ' ';
    out += yearField(t);
    return out;
}

// 21.4.4.41.2 TimeString, trailing " GMT" and all — the clause really does put
// it here rather than in TimeZoneString.
std::string timeString(double t) {
    return pad(hourFromTime(t), 2) + ":" + pad(minFromTime(t), 2) + ":" +
           pad(secFromTime(t), 2) + " GMT";
}

// 21.4.4.41.3 TimeZoneString, minus its implementation-defined zone name. The
// header of date.h says why bronze emits none.
std::string timeZoneString(double tv) {
    const double offset = localOffsetMs(tv);
    const char sign = offset >= 0.0 ? '+' : '-';
    const double abs = std::abs(offset);
    return std::string(1, sign) + pad(std::floor(abs / kMsPerHour), 2) +
           pad(std::floor(modulo(abs, kMsPerHour) / kMsPerMinute), 2);
}

double daysInMonth(double month, double year) {
    static const double kLengths[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int m = static_cast<int>(month);
    if (m != 1) return kLengths[m];
    const bool leap = daysInYear(year) == 366.0;
    return leap ? 29.0 : 28.0;
}

// ---- the parser -------------------------------------------------------------

// A cursor over the text, in the house's recursive-descent style: every
// production below either consumes what it matched or leaves the cursor where
// it found it by failing the whole parse.
struct Cursor {
    std::string_view s;
    size_t i = 0;

    bool done() const { return i >= s.size(); }
    char peek() const { return i < s.size() ? s[i] : '\0'; }
    bool eat(char c) {
        if (peek() != c) return false;
        ++i;
        return true;
    }
    bool eatLiteral(std::string_view lit) {
        if (s.substr(i, lit.size()) != lit) return false;
        i += lit.size();
        return true;
    }
    // Exactly `count` digits, as a number.
    bool fixedDigits(int count, double& out) {
        if (i + static_cast<size_t>(count) > s.size()) return false;
        double v = 0.0;
        for (int k = 0; k < count; ++k) {
            if (!isDigit(s[i + static_cast<size_t>(k)])) return false;
            v = v * 10.0 + static_cast<double>(s[i + static_cast<size_t>(k)] - '0');
        }
        i += static_cast<size_t>(count);
        out = v;
        return true;
    }
    // At least `least` digits, greedily. The `toString` year field is four
    // digits for the ordinary range and six for an expanded one, and both come
    // out of the same production.
    bool someDigits(int least, double& out) {
        const size_t start = i;
        double v = 0.0;
        while (!done() && isDigit(peek())) {
            v = v * 10.0 + static_cast<double>(peek() - '0');
            ++i;
        }
        if (i - start < static_cast<size_t>(least)) {
            i = start;
            return false;
        }
        out = v;
        return true;
    }
};

int matchName(std::string_view text, const char* const* names, int count) {
    for (int k = 0; k < count; ++k) {
        if (text == names[k]) return k;
    }
    return -1;
}

// 21.4.1.15's date-time string format, with two liberalisations that match
// every engine and cost nothing: `T` and `Z` are accepted in either case, and
// the offset's colon is optional. Nothing else is widened — in particular a
// space in place of the `T` is REFUSED further down rather than accepted,
// because that form is node's extension and not this grammar.
bool parseIso(std::string_view text, double& out) {
    Cursor c{text, 0};
    double year = 0.0;
    if (c.peek() == '+' || c.peek() == '-') {
        const bool negative = c.peek() == '-';
        ++c.i;
        double magnitude = 0.0;
        if (!c.fixedDigits(6, magnitude)) return false;
        // "-000000" names no year: 21.4.1.15 excludes it explicitly, because
        // there is no year zero to negate.
        if (negative && magnitude == 0.0) return false;
        year = negative ? -magnitude : magnitude;
    } else if (!c.fixedDigits(4, year)) {
        return false;
    }

    double month = 1.0;
    double dayOfMonth = 1.0;
    if (c.eat('-')) {
        if (!c.fixedDigits(2, month)) return false;
        if (month < 1.0 || month > 12.0) return false;
        if (c.eat('-')) {
            if (!c.fixedDigits(2, dayOfMonth)) return false;
            // Out of bounds is a parse FAILURE, not a normalisation: the note
            // under 21.4.1.15 makes an illegal value the same as a syntax
            // error, so "2020-02-31" is NaN where `new Date(2020, 1, 31)` is
            // March 2nd.
            if (dayOfMonth < 1.0 || dayOfMonth > daysInMonth(month - 1.0, year)) return false;
        }
    }

    double hour = 0.0, minute = 0.0, second = 0.0, millis = 0.0;
    bool hasTime = false;
    if (c.peek() == 'T' || c.peek() == 't') {
        ++c.i;
        hasTime = true;
        if (!c.fixedDigits(2, hour)) return false;
        if (!c.eat(':')) return false;
        if (!c.fixedDigits(2, minute)) return false;
        if (c.eat(':')) {
            if (!c.fixedDigits(2, second)) return false;
            if (c.eat('.')) {
                const size_t start = c.i;
                while (!c.done() && isDigit(c.peek())) ++c.i;
                if (c.i == start) return false;
                // The grammar spells exactly three digits. A longer fraction is
                // accepted and TRUNCATED rather than rounded, which is what
                // every engine does and what keeps a millisecond time value
                // from being nudged by digits it cannot hold.
                std::string frac(text.substr(start, c.i - start));
                frac.resize(3, '0');
                millis = (frac[0] - '0') * 100.0 + (frac[1] - '0') * 10.0 + (frac[2] - '0');
            }
        }
        if (hour > 24.0 || minute > 59.0 || second > 59.0) return false;
        // 24:00:00.000 is the one hour value past 23 the grammar allows, and
        // only as the exact end of a day.
        if (hour == 24.0 && (minute != 0.0 || second != 0.0 || millis != 0.0)) return false;
    }

    bool hasOffset = false;
    double offset = 0.0;
    if (c.peek() == 'Z' || c.peek() == 'z') {
        ++c.i;
        hasOffset = true;
    } else if (c.peek() == '+' || c.peek() == '-') {
        const bool negative = c.peek() == '-';
        ++c.i;
        double offsetHours = 0.0, offsetMinutes = 0.0;
        if (!c.fixedDigits(2, offsetHours)) return false;
        c.eat(':');
        if (!c.fixedDigits(2, offsetMinutes)) return false;
        if (offsetHours > 23.0 || offsetMinutes > 59.0) return false;
        offset = offsetHours * kMsPerHour + offsetMinutes * kMsPerMinute;
        if (negative) offset = -offset;
        hasOffset = true;
    }
    if (!c.done()) return false;

    double tv = makeDate(makeDay(year, month - 1.0, dayOfMonth),
                         makeTime(hour, minute, second, millis));
    if (hasOffset) {
        tv -= offset;
    } else if (hasTime) {
        // 21.4.1.15: a date-time form with no offset is LOCAL time, while a
        // date-only form is UTC. One grammar, two zones, and this is the line
        // where that split lives.
        tv = utcFromLocal(tv);
    }
    out = timeClip(tv);
    return true;
}

// bronze's own `toString`: "Wed Jan 01 2020 00:00:00 GMT+0000".
bool parseDateTimeString(std::string_view text, double& out) {
    Cursor c{text, 0};
    if (c.i + 3 > c.s.size()) return false;
    if (matchName(text.substr(0, 3), kWeekdayNames, 7) < 0) return false;
    c.i += 3;
    if (!c.eat(' ')) return false;
    const int month = matchName(text.substr(c.i, 3), kMonthNames, 12);
    if (month < 0) return false;
    c.i += 3;
    if (!c.eat(' ')) return false;
    double dayOfMonth = 0.0;
    if (!c.fixedDigits(2, dayOfMonth)) return false;
    if (!c.eat(' ')) return false;
    const bool negativeYear = c.eat('-');
    double year = 0.0;
    if (!c.someDigits(4, year)) return false;
    if (negativeYear) year = -year;
    if (!c.eat(' ')) return false;
    double hour = 0.0, minute = 0.0, second = 0.0;
    if (!c.fixedDigits(2, hour) || !c.eat(':')) return false;
    if (!c.fixedDigits(2, minute) || !c.eat(':')) return false;
    if (!c.fixedDigits(2, second)) return false;
    if (!c.eat(' ') || !c.eatLiteral("GMT")) return false;
    const bool negativeOffset = c.peek() == '-';
    if (!c.eat('+') && !c.eat('-')) return false;
    double offsetHours = 0.0, offsetMinutes = 0.0;
    if (!c.fixedDigits(2, offsetHours)) return false;
    if (!c.fixedDigits(2, offsetMinutes)) return false;
    if (!c.done()) return false;

    double offset = offsetHours * kMsPerHour + offsetMinutes * kMsPerMinute;
    if (negativeOffset) offset = -offset;
    const double tv = makeDate(makeDay(year, static_cast<double>(month), dayOfMonth),
                               makeTime(hour, minute, second, 0.0));
    out = timeClip(tv - offset);
    return true;
}

// bronze's own `toUTCString`: "Wed, 01 Jan 2020 00:00:00 GMT".
bool parseUtcString(std::string_view text, double& out) {
    Cursor c{text, 0};
    if (c.i + 3 > c.s.size()) return false;
    if (matchName(text.substr(0, 3), kWeekdayNames, 7) < 0) return false;
    c.i += 3;
    if (!c.eat(',') || !c.eat(' ')) return false;
    double dayOfMonth = 0.0;
    if (!c.fixedDigits(2, dayOfMonth)) return false;
    if (!c.eat(' ')) return false;
    const int month = matchName(text.substr(c.i, 3), kMonthNames, 12);
    if (month < 0) return false;
    c.i += 3;
    if (!c.eat(' ')) return false;
    const bool negativeYear = c.eat('-');
    double year = 0.0;
    if (!c.someDigits(4, year)) return false;
    if (negativeYear) year = -year;
    if (!c.eat(' ')) return false;
    double hour = 0.0, minute = 0.0, second = 0.0;
    if (!c.fixedDigits(2, hour) || !c.eat(':')) return false;
    if (!c.fixedDigits(2, minute) || !c.eat(':')) return false;
    if (!c.fixedDigits(2, second)) return false;
    if (!c.eat(' ') || !c.eatLiteral("GMT")) return false;
    if (!c.done()) return false;

    const double tv = makeDate(makeDay(year, static_cast<double>(month), dayOfMonth),
                               makeTime(hour, minute, second, 0.0));
    out = timeClip(tv);
    return true;
}

std::string lowered(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// The formats node accepts and bronze does not. Recognising one is not an
// attempt to parse it — it is the evidence needed to refuse BY NAME, because a
// string that is obviously a date must never come back as NaN just because this
// parser is narrower than V8's. A string these do not recognise really is not a
// date, and NaN is then 21.4.3.2's own answer.
bool refusedFormatOf(std::string_view text, std::string& name) {
    const std::string low = lowered(text);
    if (low.find('/') != std::string::npos) {
        name = "the slash-separated date format (e.g. \"1/1/2020\" or \"2020/01/01\")";
        return true;
    }
    // A leading month name, in either spelling: "Jan 1 2020", "January 1, 2020".
    // Matched case-insensitively, because that is how the engines that accept
    // the format match it — the refusal has to cover every spelling a program
    // would otherwise get a wrong answer for, not just the tidy one.
    //
    // The name must be FOLLOWED by a separator, so that "mayonnaise" is not a
    // date and still answers NaN: a refusal is a hard error, and one earned by
    // a prefix coincidence would stop a program over a string nobody meant as a
    // date.
    auto separatorAfter = [&low](size_t at) {
        if (at >= low.size()) return false;
        const char c = low[at];
        return c == ' ' || c == ',' || c == '.';
    };
    if (low.size() >= 3) {
        for (const char* full : kFullMonthNames) {
            const std::string_view spelled{full};
            if (low.size() > spelled.size() && low.compare(0, spelled.size(), full) == 0 &&
                separatorAfter(spelled.size())) {
                name = "the month-name-first date format (e.g. \"January 1, 2020\")";
                return true;
            }
        }
        for (const char* abbrev : kMonthNames) {
            if (low.compare(0, 3, lowered(abbrev)) == 0 && separatorAfter(3)) {
                name = "the month-name-first date format (e.g. \"Jan 1 2020\")";
                return true;
            }
        }
    }
    // "2020-01-01 00:00:00" — the ISO date and time joined by a space instead
    // of a `T`. Not in 21.4.1.15's grammar; universally accepted in practice.
    {
        Cursor c{text, 0};
        double y = 0.0, m = 0.0, d = 0.0;
        if (c.fixedDigits(4, y) && c.eat('-') && c.fixedDigits(2, m) && c.eat('-') &&
            c.fixedDigits(2, d) && c.eat(' ')) {
            name = "the space-separated ISO date-time format (e.g. \"2020-01-01 00:00:00\"); "
                   "write the `T` that ECMA-262 21.4.1.15 requires";
            return true;
        }
    }
    // A leading weekday name that neither round-trip parser accepted: the
    // RFC 2822 family, which covers `toUTCString` variants with a numeric
    // offset and `toString` output carrying a parenthesised zone name.
    if (text.size() > 3 && matchName(text.substr(0, 3), kWeekdayNames, 7) >= 0 &&
        separatorAfter(3)) {
        name = "the RFC 2822 date-time format (e.g. \"Wed, 01 Jan 2020 00:00:00 +0000\"), and "
               "the parenthesised time-zone-name suffix other engines print";
        return true;
    }
    return false;
}

}  // namespace

bool isoString(double t, std::string& out) {
    if (std::isnan(t)) return false;
    const double y = yearFromTime(t);
    std::string year;
    // 21.4.4.36: a year outside 0000-9999 is written in the EXPANDED form, six
    // digits with a mandatory sign — which is the only way an ISO string can
    // name year 275760 or a year BC at all.
    if (y < 0.0) {
        year = "-" + pad(-y, 6);
    } else if (y > 9999.0) {
        year = "+" + pad(y, 6);
    } else {
        year = pad(y, 4);
    }
    out = year + "-" + pad(monthFromTime(t) + 1.0, 2) + "-" + pad(dateFromTime(t), 2) + "T" +
          pad(hourFromTime(t), 2) + ":" + pad(minFromTime(t), 2) + ":" +
          pad(secFromTime(t), 2) + "." + pad(msFromTime(t), 3) + "Z";
    return true;
}

std::string dateTimeString(double tv) {
    if (std::isnan(tv)) return "Invalid Date";
    const double t = localTime(tv);
    return dateString(t) + " " + timeString(t) + timeZoneString(tv);
}

std::string dateOnlyString(double tv) {
    if (std::isnan(tv)) return "Invalid Date";
    return dateString(localTime(tv));
}

std::string timeOnlyString(double tv) {
    if (std::isnan(tv)) return "Invalid Date";
    return timeString(localTime(tv)) + timeZoneString(tv);
}

std::string utcString(double tv) {
    if (std::isnan(tv)) return "Invalid Date";
    return std::string(kWeekdayNames[static_cast<int>(weekDay(tv))]) + ", " +
           pad(dateFromTime(tv), 2) + " " + kMonthNames[static_cast<int>(monthFromTime(tv))] +
           " " + yearField(tv) + " " + timeString(tv);
}

std::string inspectString(double tv) {
    std::string out;
    if (!isoString(tv, out)) return "Invalid Date";
    return out;
}

ParseOutcome parse(std::string_view text, double& out, std::string& refusedFormat) {
    // Surrounding whitespace is not in the grammar, and every engine strips it
    // before parsing. Stripping is not a format extension: nothing about the
    // string's meaning changes, so there is nothing here to diverge about.
    size_t begin = 0;
    size_t end = text.size();
    auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    while (begin < end && isSpace(text[begin])) ++begin;
    while (end > begin && isSpace(text[end - 1])) --end;
    const std::string_view trimmed = text.substr(begin, end - begin);
    if (trimmed.empty()) return ParseOutcome::NotADate;

    if (parseIso(trimmed, out)) return ParseOutcome::Ok;
    if (parseDateTimeString(trimmed, out)) return ParseOutcome::Ok;
    if (parseUtcString(trimmed, out)) return ParseOutcome::Ok;
    if (refusedFormatOf(trimmed, refusedFormat)) return ParseOutcome::RefusedFormat;
    return ParseOutcome::NotADate;
}

}  // namespace bronze::runtime::datetime
