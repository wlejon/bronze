// The Date TEXT formats: what §21.4.4's string methods and `Intl.DateTimeFormat`
// print, plus IANA / offset `timeZone` resolution and wall-clock field mapping.
//
// `Date.parse` lives in `date_parse.cpp`.

#include "runtime/date.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace bronze::runtime::datetime {

namespace {

const char* const kWeekdayShort[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* const kWeekdayLong[7] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                     "Thursday", "Friday", "Saturday"};
const char* const kWeekdayNarrow[7] = {"S", "M", "T", "W", "T", "F", "S"};

const char* const kMonthShort[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
const char* const kMonthLong[12] = {"January", "February", "March", "April", "May", "June",
                                    "July", "August", "September", "October", "November",
                                    "December"};
const char* const kMonthNarrow[12] = {"J", "F", "M", "A", "M", "J",
                                      "J", "A", "S", "O", "N", "D"};

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
    std::string out = kWeekdayShort[static_cast<int>(weekDay(t))];
    out += ' ';
    out += kMonthShort[static_cast<int>(monthFromTime(t))];
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

char asciiToLower(char c) {
    if (c >= 'A' && c <= 'Z') return static_cast<char>(c + ('a' - 'A'));
    return c;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (asciiToLower(a[i]) != asciiToLower(b[i])) return false;
    }
    return true;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Parses an explicit ISO 8601 UTC offset (`+HH:MM`, `-HH:MM`, `+HHMM`, `-HHMM`,
// `+HH`, `-HH`) and writes its canonical `"[+-]HH:MM"` form + millisecond offset.
bool parseIsoOffsetTimeZone(std::string_view s, std::string& canonicalOut, double& offsetMsOut) {
    if (s.size() < 3) return false;
    const char signChar = s[0];
    if (signChar != '+' && signChar != '-') return false;
    int hh = -1;
    int mm = -1;
    if (s.size() == 3 && isDigit(s[1]) && isDigit(s[2])) {
        hh = (s[1] - '0') * 10 + (s[2] - '0');
        mm = 0;
    } else if (s.size() == 5 && isDigit(s[1]) && isDigit(s[2]) && isDigit(s[3]) && isDigit(s[4])) {
        hh = (s[1] - '0') * 10 + (s[2] - '0');
        mm = (s[3] - '0') * 10 + (s[4] - '0');
    } else if (s.size() == 6 && isDigit(s[1]) && isDigit(s[2]) && s[3] == ':' &&
               isDigit(s[4]) && isDigit(s[5])) {
        hh = (s[1] - '0') * 10 + (s[2] - '0');
        mm = (s[4] - '0') * 10 + (s[5] - '0');
    } else {
        return false;
    }
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;
    const int totalMinutes = hh * 60 + mm;
    const char normSign = (totalMinutes == 0) ? '+' : signChar;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%c%02d:%02d", normSign, hh, mm);
    canonicalOut = buf;
    const double magMs = static_cast<double>(hh) * kMsPerHour +
                         static_cast<double>(mm) * kMsPerMinute;
    offsetMsOut = (normSign == '-') ? -magMs : magMs;
    return true;
}

// Parses `Etc/GMT+N` or `Etc/GMT-N` (POSIX sign inversion: `Etc/GMT-9` is UTC+9,
// `Etc/GMT+5` is UTC-5). `Etc/GMT+0`, `Etc/GMT-0`, `Etc/GMT0` canonicalize to `"UTC"`.
bool parseEtcGmtTimeZone(std::string_view s, std::string& canonicalOut, double& offsetMsOut) {
    constexpr std::string_view kPrefix = "etc/gmt";
    if (s.size() <= kPrefix.size()) return false;
    if (!iequals(s.substr(0, kPrefix.size()), kPrefix)) return false;
    std::string_view rest = s.substr(kPrefix.size());
    if (rest == "0" || rest == "+0" || rest == "-0") {
        canonicalOut = "UTC";
        offsetMsOut = 0.0;
        return true;
    }
    if (rest.size() < 2 || rest.size() > 3) return false;
    const char signChar = rest[0];
    if (signChar != '+' && signChar != '-') return false;
    int hours = 0;
    for (size_t i = 1; i < rest.size(); ++i) {
        if (!isDigit(rest[i])) return false;
        hours = hours * 10 + (rest[i] - '0');
    }
    // IANA tzdb supports Etc/GMT-14 .. Etc/GMT+12 (up to 14 on the minus side, 12 on the plus side;
    // ECMA-402 accepts 1..14 for Etc/GMT-N and 1..12 for Etc/GMT+N, or 1..14 generally).
    if (hours < 1 || hours > 14) return false;
    if (rest.size() == 3 && rest[1] == '0') return false;  // Reject leading zero like Etc/GMT+05
    canonicalOut = std::string("Etc/GMT") + signChar + std::to_string(hours);
    // POSIX sign inversion: Etc/GMT-9 is +09:00, Etc/GMT+5 is -05:00.
    offsetMsOut = (signChar == '-') ? (static_cast<double>(hours) * kMsPerHour)
                                    : (-static_cast<double>(hours) * kMsPerHour);
    return true;
}

struct IanaZoneEntry {
    const char* alias;
    const char* canonical;
    int fallbackStandardOffsetMinutes;
};

// Built-in canonical & alias table so case-insensitive matching and fallback
// offsets work even on minimal systems without `/usr/share/zoneinfo`, while
// C++20 `std::chrono::locate_zone` supplies full historical + DST transitions
// when tzdata is present.
constexpr IanaZoneEntry kKnownIanaZones[] = {
    {"Asia/Tokyo", "Asia/Tokyo", 540},
    {"Japan", "Asia/Tokyo", 540},
    {"Asia/Seoul", "Asia/Seoul", 540},
    {"ROK", "Asia/Seoul", 540},
    {"Asia/Shanghai", "Asia/Shanghai", 480},
    {"Asia/Chongqing", "Asia/Shanghai", 480},
    {"PRC", "Asia/Shanghai", 480},
    {"Asia/Hong_Kong", "Asia/Hong_Kong", 480},
    {"Hongkong", "Asia/Hong_Kong", 480},
    {"Asia/Taipei", "Asia/Taipei", 480},
    {"Asia/Singapore", "Asia/Singapore", 480},
    {"Singapore", "Asia/Singapore", 480},
    {"Asia/Manila", "Asia/Manila", 480},
    {"Asia/Bangkok", "Asia/Bangkok", 420},
    {"Asia/Ho_Chi_Minh", "Asia/Ho_Chi_Minh", 420},
    {"Asia/Saigon", "Asia/Ho_Chi_Minh", 420},
    {"Asia/Jakarta", "Asia/Jakarta", 420},
    {"Asia/Kolkata", "Asia/Kolkata", 330},
    {"Asia/Calcutta", "Asia/Kolkata", 330},
    {"Asia/Kathmandu", "Asia/Kathmandu", 345},
    {"Asia/Katmandu", "Asia/Kathmandu", 345},
    {"Asia/Dhaka", "Asia/Dhaka", 360},
    {"Asia/Karachi", "Asia/Karachi", 300},
    {"Asia/Tashkent", "Asia/Tashkent", 300},
    {"Asia/Dubai", "Asia/Dubai", 240},
    {"Asia/Tehran", "Asia/Tehran", 210},
    {"Asia/Baghdad", "Asia/Baghdad", 180},
    {"Asia/Riyadh", "Asia/Riyadh", 180},
    {"Asia/Jerusalem", "Asia/Jerusalem", 120},
    {"Israel", "Asia/Jerusalem", 120},
    {"Europe/London", "Europe/London", 0},
    {"GB", "Europe/London", 0},
    {"Europe/Dublin", "Europe/Dublin", 0},
    {"Europe/Lisbon", "Europe/Lisbon", 0},
    {"Europe/Paris", "Europe/Paris", 60},
    {"Europe/Berlin", "Europe/Berlin", 60},
    {"Europe/Madrid", "Europe/Madrid", 60},
    {"Europe/Rome", "Europe/Rome", 60},
    {"Europe/Amsterdam", "Europe/Amsterdam", 60},
    {"Europe/Brussels", "Europe/Brussels", 60},
    {"Europe/Vienna", "Europe/Vienna", 60},
    {"Europe/Zurich", "Europe/Zurich", 60},
    {"Europe/Stockholm", "Europe/Stockholm", 60},
    {"Europe/Oslo", "Europe/Oslo", 60},
    {"Europe/Copenhagen", "Europe/Copenhagen", 60},
    {"Europe/Warsaw", "Europe/Warsaw", 60},
    {"Europe/Prague", "Europe/Prague", 60},
    {"Europe/Budapest", "Europe/Budapest", 60},
    {"Europe/Athens", "Europe/Athens", 120},
    {"Europe/Helsinki", "Europe/Helsinki", 120},
    {"Europe/Bucharest", "Europe/Bucharest", 120},
    {"Europe/Kyiv", "Europe/Kyiv", 120},
    {"Europe/Kiev", "Europe/Kyiv", 120},
    {"Europe/Istanbul", "Europe/Istanbul", 180},
    {"Europe/Moscow", "Europe/Moscow", 180},
    {"Africa/Cairo", "Africa/Cairo", 120},
    {"Egypt", "Africa/Cairo", 120},
    {"Africa/Johannesburg", "Africa/Johannesburg", 120},
    {"Africa/Lagos", "Africa/Lagos", 60},
    {"Africa/Nairobi", "Africa/Nairobi", 180},
    {"Africa/Casablanca", "Africa/Casablanca", 60},
    {"America/New_York", "America/New_York", -300},
    {"US/Eastern", "America/New_York", -300},
    {"America/Toronto", "America/Toronto", -300},
    {"America/Chicago", "America/Chicago", -360},
    {"US/Central", "America/Chicago", -360},
    {"America/Winnipeg", "America/Winnipeg", -360},
    {"America/Mexico_City", "America/Mexico_City", -360},
    {"America/Denver", "America/Denver", -420},
    {"US/Mountain", "America/Denver", -420},
    {"America/Phoenix", "America/Phoenix", -420},
    {"US/Arizona", "America/Phoenix", -420},
    {"America/Edmonton", "America/Edmonton", -420},
    {"America/Los_Angeles", "America/Los_Angeles", -480},
    {"US/Pacific", "America/Los_Angeles", -480},
    {"America/Vancouver", "America/Vancouver", -480},
    {"America/Anchorage", "America/Anchorage", -540},
    {"US/Alaska", "America/Anchorage", -540},
    {"Pacific/Honolulu", "Pacific/Honolulu", -600},
    {"US/Hawaii", "Pacific/Honolulu", -600},
    {"America/Halifax", "America/Halifax", -240},
    {"America/St_Johns", "America/St_Johns", -210},
    {"America/Sao_Paulo", "America/Sao_Paulo", -180},
    {"America/Argentina/Buenos_Aires", "America/Argentina/Buenos_Aires", -180},
    {"America/Buenos_Aires", "America/Argentina/Buenos_Aires", -180},
    {"America/Santiago", "America/Santiago", -240},
    {"America/Bogota", "America/Bogota", -300},
    {"America/Lima", "America/Lima", -300},
    {"America/Caracas", "America/Caracas", -240},
    {"Australia/Sydney", "Australia/Sydney", 600},
    {"Australia/Melbourne", "Australia/Melbourne", 600},
    {"Australia/Brisbane", "Australia/Brisbane", 600},
    {"Australia/Adelaide", "Australia/Adelaide", 570},
    {"Australia/Darwin", "Australia/Darwin", 570},
    {"Australia/Perth", "Australia/Perth", 480},
    {"Australia/Hobart", "Australia/Hobart", 600},
    {"Pacific/Auckland", "Pacific/Auckland", 720},
    {"NZ", "Pacific/Auckland", 720},
    {"Pacific/Fiji", "Pacific/Fiji", 720},
    {"Pacific/Guam", "Pacific/Guam", 600},
    {"Atlantic/Reykjavik", "Atlantic/Reykjavik", 0},
    {"Atlantic/Azores", "Atlantic/Azores", -60},
};

const IanaZoneEntry* findKnownIanaZone(std::string_view input) {
    for (const IanaZoneEntry& entry : kKnownIanaZones) {
        if (iequals(input, entry.alias) || iequals(input, entry.canonical)) {
            return &entry;
        }
    }
    return nullptr;
}

bool looksLikeIanaIdentifier(std::string_view s) {
    if (s.empty() || s.size() > 64) return false;
    // Reject control chars, spaces, or leading/trailing slashes.
    if (s.front() == '/' || s.back() == '/') return false;
    for (char c : s) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '/' || c == '_' || c == '-' || c == '+';
        if (!ok) return false;
    }
    return true;
}

}  // namespace

bool resolveTimeZone(std::string_view input, std::string& canonicalOut) {
    if (input.empty()) return false;

    // 1. UTC / GMT synonyms (ECMA-402 CanonicalizeTimeZoneName).
    if (iequals(input, "UTC") || iequals(input, "GMT") || iequals(input, "Etc/UTC") ||
        iequals(input, "Etc/GMT") || iequals(input, "Etc/UCT") || iequals(input, "Z") ||
        iequals(input, "UCT") || iequals(input, "Universal") || iequals(input, "Zulu") ||
        iequals(input, "Greenwich") || iequals(input, "GMT0") || iequals(input, "GMT+0") ||
        iequals(input, "GMT-0")) {
        canonicalOut = "UTC";
        return true;
    }

    // 2. Explicit ISO 8601 offset string (`+HH:MM`, `-HH:MM`, `+HHMM`, `+HH`, etc.).
    double unusedOffset = 0.0;
    if (parseIsoOffsetTimeZone(input, canonicalOut, unusedOffset)) {
        return true;
    }

    // 3. `Etc/GMT+N` / `Etc/GMT-N` (POSIX sign inversion).
    if (parseEtcGmtTimeZone(input, canonicalOut, unusedOffset)) {
        return true;
    }

    if (!looksLikeIanaIdentifier(input)) return false;

    // 4. Known IANA zone or alias (case-insensitive).
    if (const IanaZoneEntry* known = findKnownIanaZone(input)) {
        canonicalOut = known->canonical;
        return true;
    }

    // 5. System IANA tzdb via C++20 `std::chrono::locate_zone`.
#if !defined(_LIBCPP_VERSION) && defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
    try {
        const auto& tzdb = std::chrono::get_tzdb();
        for (const auto& z : tzdb.zones) {
            if (iequals(input, z.name())) {
                canonicalOut = std::string(z.name());
                return true;
            }
        }
        for (const auto& link : tzdb.links) {
            if (iequals(input, link.name())) {
                canonicalOut = std::string(link.target());
                return true;
            }
        }
    } catch (...) {
        // Fall through if platform tzdb is unavailable.
    }
#endif

    return false;
}

std::string defaultTimeZone() {
#if !defined(_LIBCPP_VERSION) && defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
    try {
        const auto* z = std::chrono::current_zone();
        if (z) {
            std::string canonical;
            if (resolveTimeZone(z->name(), canonical)) return canonical;
            if (!z->name().empty()) return std::string(z->name());
        }
    } catch (...) {
    }
#endif
    return "UTC";
}

double timeZoneOffsetMs(double tUtc, std::string_view canonicalZone) {
    if (!std::isfinite(tUtc)) return 0.0;
    if (canonicalZone.empty()) return localOffsetMs(tUtc);
    if (canonicalZone == "UTC") return 0.0;

    std::string dummy;
    double fixedOffsetMs = 0.0;
    if (parseIsoOffsetTimeZone(canonicalZone, dummy, fixedOffsetMs)) {
        return fixedOffsetMs;
    }
    if (parseEtcGmtTimeZone(canonicalZone, dummy, fixedOffsetMs)) {
        return fixedOffsetMs;
    }

#if !defined(_LIBCPP_VERSION) && defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
    // Clamp query instant into [-2208988800, 4102444800] (1900-01-01 .. 2100-01-01)
    // so extreme JavaScript dates (±275760) do not overflow chrono sys_seconds.
    double secs = std::floor(tUtc / kMsPerSecond);
    constexpr double kMinTzdbSeconds = -2208988800.0;
    constexpr double kMaxTzdbSeconds = 4102444800.0;
    if (secs < kMinTzdbSeconds) secs = kMinTzdbSeconds;
    if (secs > kMaxTzdbSeconds) secs = kMaxTzdbSeconds;

    try {
        const auto* z = std::chrono::locate_zone(canonicalZone);
        if (z) {
            const auto tp = std::chrono::sys_seconds{
                std::chrono::seconds{static_cast<int64_t>(secs)}};
            const auto info = z->get_info(tp);
            return static_cast<double>(info.offset.count()) * kMsPerSecond;
        }
    } catch (...) {
    }
#endif

    if (const IanaZoneEntry* known = findKnownIanaZone(canonicalZone)) {
        return static_cast<double>(known->fallbackStandardOffsetMinutes) * kMsPerMinute;
    }
    return 0.0;
}

double zonedTime(double tUtc, std::string_view canonicalZone) {
    if (!std::isfinite(tUtc)) return tUtc;
    return tUtc + timeZoneOffsetMs(tUtc, canonicalZone);
}

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
    return std::string(kWeekdayShort[static_cast<int>(weekDay(tv))]) + ", " +
           pad(dateFromTime(tv), 2) + " " + kMonthShort[static_cast<int>(monthFromTime(tv))] +
           " " + yearField(tv) + " " + timeString(tv);
}

void applyDateTimeDefaults(DateTimeFormatOptions& opts, bool defaultDate, bool defaultTime) {
    if (!opts.dateStyle.empty() || !opts.timeStyle.empty()) {
        if (!opts.dateStyle.empty()) {
            opts.year = (opts.dateStyle == "short") ? "2-digit" : "numeric";
            if (opts.dateStyle == "full" || opts.dateStyle == "long") {
                opts.month = "long";
            } else if (opts.dateStyle == "medium") {
                opts.month = "short";
            } else {
                opts.month = "numeric";
            }
            opts.day = "numeric";
            if (opts.dateStyle == "full") opts.weekday = "long";
        }
        if (!opts.timeStyle.empty()) {
            opts.hour = "numeric";
            opts.minute = "2-digit";
            if (opts.timeStyle != "short") opts.second = "2-digit";
            if (opts.timeStyle == "full" || opts.timeStyle == "long") {
                opts.timeZoneName = "short";
            }
        }
        return;
    }

    const bool hasAnyDate = !opts.weekday.empty() || !opts.year.empty() ||
                            !opts.month.empty() || !opts.day.empty();
    const bool hasAnyTime = !opts.hour.empty() || !opts.minute.empty() ||
                            !opts.second.empty();

    if (!hasAnyDate && !hasAnyTime) {
        if (defaultDate) {
            opts.year = "numeric";
            opts.month = "numeric";
            opts.day = "numeric";
        }
        if (defaultTime) {
            opts.hour = "numeric";
            opts.minute = "2-digit";
            opts.second = "2-digit";
        }
    }
}

namespace {

std::string formatGmtOffsetName(double offsetMs, bool longForm) {
    if (offsetMs == 0.0 && !longForm) return "UTC";
    if (offsetMs == 0.0 && longForm) return "GMT+00:00";
    const char sign = offsetMs >= 0.0 ? '+' : '-';
    const double absMs = std::abs(offsetMs);
    const int hh = static_cast<int>(std::floor(absMs / kMsPerHour));
    const int mm = static_cast<int>(std::floor(modulo(absMs, kMsPerHour) / kMsPerMinute));
    char buf[32];
    if (longForm || mm != 0) {
        std::snprintf(buf, sizeof(buf), "GMT%c%02d:%02d", sign, hh, mm);
    } else {
        std::snprintf(buf, sizeof(buf), "GMT%c%d", sign, hh);
    }
    return buf;
}

}  // namespace

bool formatDateTimeParts(double tUtc, const DateTimeFormatOptions& rawOpts,
                         std::vector<DateTimePart>& out) {
    out.clear();
    const double clipped = timeClip(tUtc);
    if (std::isnan(clipped)) return false;

    DateTimeFormatOptions opts = rawOpts;
    applyDateTimeDefaults(opts, /*defaultDate=*/true, /*defaultTime=*/false);

    const double offsetMs = timeZoneOffsetMs(clipped, opts.timeZone);
    const double t = clipped + offsetMs;

    const int wd = static_cast<int>(weekDay(t));
    const int y = static_cast<int>(yearFromTime(t));
    const int m0 = static_cast<int>(monthFromTime(t));
    const int d = static_cast<int>(dateFromTime(t));
    const int h24 = static_cast<int>(hourFromTime(t));
    const int mi = static_cast<int>(minFromTime(t));
    const int sc = static_cast<int>(secFromTime(t));

    auto push = [&](const char* type, std::string value) {
        out.push_back(DateTimePart{type, std::move(value)});
    };

    bool emittedAny = false;

    if (!opts.weekday.empty()) {
        const char* w = kWeekdayShort[wd];
        if (opts.weekday == "long") w = kWeekdayLong[wd];
        else if (opts.weekday == "narrow") w = kWeekdayNarrow[wd];
        push("weekday", w);
        emittedAny = true;
    }

    const bool hasDateYMD = !opts.year.empty() || !opts.month.empty() || !opts.day.empty();
    if (hasDateYMD) {
        if (emittedAny) push("literal", ", ");
        const bool textMonth =
            (opts.month == "short" || opts.month == "long" || opts.month == "narrow");
        bool firstDateField = true;
        if (!opts.month.empty()) {
            if (opts.month == "2-digit") {
                push("month", pad(m0 + 1, 2));
            } else if (opts.month == "long") {
                push("month", kMonthLong[m0]);
            } else if (opts.month == "short") {
                push("month", kMonthShort[m0]);
            } else if (opts.month == "narrow") {
                push("month", kMonthNarrow[m0]);
            } else {
                push("month", std::to_string(m0 + 1));
            }
            firstDateField = false;
        }
        if (!opts.day.empty()) {
            if (!firstDateField) push("literal", textMonth ? " " : "/");
            if (opts.day == "2-digit") {
                push("day", pad(d, 2));
            } else {
                push("day", std::to_string(d));
            }
            firstDateField = false;
        }
        if (!opts.year.empty()) {
            if (!firstDateField) push("literal", textMonth ? ", " : "/");
            const int absY = std::abs(y);
            if (opts.year == "2-digit") {
                push("year", pad(absY % 100, 2));
            } else {
                push("year", std::to_string(y));
            }
        }
        emittedAny = true;
    }

    const bool hasTime = !opts.hour.empty() || !opts.minute.empty() || !opts.second.empty();
    if (hasTime) {
        if (emittedAny) push("literal", ", ");
        bool use12 = opts.hour12;
        if (!opts.hasHour12 && !opts.hourCycle.empty()) {
            use12 = (opts.hourCycle == "h11" || opts.hourCycle == "h12");
        }
        bool firstTimeField = true;
        if (!opts.hour.empty()) {
            int dispHour = h24;
            if (use12) {
                if (opts.hourCycle == "h11") {
                    dispHour = h24 % 12;
                } else {
                    dispHour = h24 % 12;
                    if (dispHour == 0) dispHour = 12;
                }
            } else if (opts.hourCycle == "h24" && h24 == 0) {
                dispHour = 24;
            }
            if (opts.hour == "2-digit") {
                push("hour", pad(dispHour, 2));
            } else {
                push("hour", std::to_string(dispHour));
            }
            firstTimeField = false;
        }
        if (!opts.minute.empty()) {
            if (!firstTimeField) push("literal", ":");
            if (opts.minute == "2-digit" || !opts.hour.empty()) {
                push("minute", pad(mi, 2));
            } else {
                push("minute", std::to_string(mi));
            }
            firstTimeField = false;
        }
        if (!opts.second.empty()) {
            if (!firstTimeField) push("literal", ":");
            if (opts.second == "2-digit" || !opts.minute.empty()) {
                push("second", pad(sc, 2));
            } else {
                push("second", std::to_string(sc));
            }
        }
        if (use12 && !opts.hour.empty()) {
            push("literal", " ");
            push("dayPeriod", (h24 >= 12) ? "PM" : "AM");
        }
        emittedAny = true;
    }

    if (!opts.timeZoneName.empty()) {
        if (emittedAny) push("literal", " ");
        const bool longOffset =
            (opts.timeZoneName == "longOffset" || opts.timeZoneName == "long");
        push("timeZoneName", formatGmtOffsetName(offsetMs, longOffset));
    }

    return true;
}

bool formatDateTime(double tUtc, const DateTimeFormatOptions& opts, std::string& out) {
    std::vector<DateTimePart> parts;
    if (!formatDateTimeParts(tUtc, opts, parts)) return false;
    out.clear();
    for (const DateTimePart& p : parts) {
        out += p.value;
    }
    return true;
}

std::string localeDateString(double tv, std::string_view canonicalZone) {
    if (std::isnan(tv)) return "Invalid Date";
    DateTimeFormatOptions opts;
    opts.timeZone = std::string(canonicalZone);
    applyDateTimeDefaults(opts, /*defaultDate=*/true, /*defaultTime=*/false);
    std::string out;
    if (!formatDateTime(tv, opts, out)) return "Invalid Date";
    return out;
}

std::string localeTimeString(double tv, std::string_view canonicalZone) {
    if (std::isnan(tv)) return "Invalid Date";
    DateTimeFormatOptions opts;
    opts.timeZone = std::string(canonicalZone);
    applyDateTimeDefaults(opts, /*defaultDate=*/false, /*defaultTime=*/true);
    std::string out;
    if (!formatDateTime(tv, opts, out)) return "Invalid Date";
    return out;
}

std::string localeDateTimeString(double tv, std::string_view canonicalZone) {
    if (std::isnan(tv)) return "Invalid Date";
    DateTimeFormatOptions opts;
    opts.timeZone = std::string(canonicalZone);
    applyDateTimeDefaults(opts, /*defaultDate=*/true, /*defaultTime=*/true);
    std::string out;
    if (!formatDateTime(tv, opts, out)) return "Invalid Date";
    return out;
}

std::string inspectString(double tv) {
    std::string out;
    if (!isoString(tv, out)) return "Invalid Date";
    return out;
}

}  // namespace bronze::runtime::datetime
