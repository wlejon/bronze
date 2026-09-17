// The Date TEXT formats: what §21.4.4's five string methods print, and what
// §21.4.3.2 `Date.parse` reads back.
//
// Formatting and parsing live in one file because they are one contract. Every
// string this half prints, the other half must accept — `Date.parse(d.toString())`
// and `Date.parse(d.toISOString())` are round-trips a program is entitled to —
// and splitting them is how a padding change on one side becomes a parse
// failure on the other.

#include "runtime/date.h"

#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace bronze::runtime::datetime {

namespace {

const char* const kWeekdayNames[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* const kMonthNames[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

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

// ---- the parser -------------------------------------------------------------
//
// 21.4.3.2 pins exactly one grammar — 21.4.1.15's date-time string format —
// and says that any other string "may fall back to any implementation-specific
// heuristics or implementation-specific date formats". Every program was
// written against V8's heuristics, and a program that calls
// `Date.parse("Jan 1 2020")` or `new Date("1/2/2020")` is entitled to the
// answer node gives it, wrong-looking corners included: "Jan 1" is the year
// 2001 (a missing day component fills as 1 and the 1 becomes the year), "0"
// is the year 2000, "Feb 31 2020" is March 2nd, and a leading space turns an
// ISO date-only string from UTC into local time because the legacy loop takes
// over. So what follows is V8's `DateParser` (src/date/dateparser*.{h,cc}),
// carried over step for step — the tokenizer, the ES5 pass, the legacy loop and
// the three composers — with the names kept close enough that a divergence can
// be checked against the original line by line. Nothing here is "what a date
// parser should do"; it is what the engine every program was tested against
// does, which is the only correctness there is for a heuristic.
//
// The shape, from V8's own comment:
//   ES5 ISO 8601 dates:
//     [('-'|'+')yy]yyyy[-MM[-DD]][THH:mm[:ss[.sss]][Z|(+|-)hh:mm]]
//     with sss allowed more or fewer than three digits, hh:mm also as hhmm,
//     and a missing zone meaning UTC for a date-only form, local otherwise.
//   Legacy dates:
//     any unrecognised word before the first number is ignored; parenthesised
//     text is ignored; a number followed by ':' is a time, by '::' a time with
//     a zero second, by '.' a time that must be followed by milliseconds; any
//     other number is a date component; a word starting with a month's first
//     three letters names the month; a zone word or a '(+|-)(hhmm|hh:)' after
//     a time or UTC is an offset; extra signs or an unmatched ')' after the
//     first number fail the parse.
//   A string both grammars accept (e.g. 1970-01-01) is the ES5 reading.

constexpr int kNone = INT_MAX;
constexpr int kMaxSignificantDigits = 9;

bool between(int x, int lo, int hi) {
    return static_cast<unsigned>(x - lo) <= static_cast<unsigned>(hi - lo);
}

bool isAsciiDigitCp(uint32_t c) { return c >= '0' && c <= '9'; }

// V8's IsWhiteSpace: the Unicode Zs category plus tab, VT, FF and the BOM. Line
// terminators are deliberately NOT here; the tokenizer treats them below.
bool isWhiteSpaceCp(uint32_t c) {
    switch (c) {
        case 0x09: case 0x0B: case 0x0C: case 0x20: case 0xA0: case 0x1680:
        case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004: case 0x2005:
        case 0x2006: case 0x2007: case 0x2008: case 0x2009: case 0x200A:
        case 0x202F: case 0x205F: case 0x3000: case 0xFEFF:
            return true;
        default:
            return false;
    }
}

bool isWhiteSpaceOrLineTerminatorCp(uint32_t c) {
    return isWhiteSpaceCp(c) || c == 0x0A || c == 0x0D || c == 0x2028 || c == 0x2029;
}

uint32_t asciiAlphaToLower(uint32_t c) {
    // V8's AsciiAlphaToLower is the bit trick, applied to EVERY code point —
    // so a non-letter's prefix is perturbed the same way it is there.
    return c | 0x20;
}

// V8's InputReader: the current code point in `ch`, 0 at the end (which is
// also what a NUL in the text reads as, so a NUL ends the parse there too).
// The text arrives as UTF-8 and is decoded here, because V8 walks code units
// and the keyword and whitespace tests below are per character, not per byte.
class Input {
public:
    explicit Input(std::string_view s) : s_(s) { next(); }

    int position() const { return count_; }

    void next() {
        ++count_;
        if (i_ >= s_.size()) {
            ch_ = 0;
            return;
        }
        const auto byte = [&](size_t k) { return static_cast<uint32_t>(static_cast<unsigned char>(s_[k])); };
        const uint32_t b0 = byte(i_);
        if (b0 < 0x80) {
            ch_ = b0;
            i_ += 1;
        } else if ((b0 & 0xE0) == 0xC0 && i_ + 1 < s_.size()) {
            ch_ = ((b0 & 0x1F) << 6) | (byte(i_ + 1) & 0x3F);
            i_ += 2;
        } else if ((b0 & 0xF0) == 0xE0 && i_ + 2 < s_.size()) {
            ch_ = ((b0 & 0x0F) << 12) | ((byte(i_ + 1) & 0x3F) << 6) | (byte(i_ + 2) & 0x3F);
            i_ += 3;
        } else if ((b0 & 0xF8) == 0xF0 && i_ + 3 < s_.size()) {
            ch_ = ((b0 & 0x07) << 18) | ((byte(i_ + 1) & 0x3F) << 12) |
                  ((byte(i_ + 2) & 0x3F) << 6) | (byte(i_ + 3) & 0x3F);
            i_ += 4;
        } else {
            ch_ = 0xFFFD;
            i_ += 1;
        }
    }

    // Leading zeros are skipped and do not count as significant; past nine
    // significant digits the rest are consumed but dropped. The token's
    // LENGTH still counts every character, which is how "000010" reads as
    // the number 10 with length 6 and so is not a fixed-length-2 hour.
    int readUnsignedNumeral() {
        int n = 0;
        int i = 0;
        while (ch_ == '0') next();
        while (isAsciiDigit()) {
            if (i < kMaxSignificantDigits) n = n * 10 + static_cast<int>(ch_ - '0');
            i++;
            next();
        }
        return n;
    }

    int readWord(uint32_t* prefix, int prefixSize) {
        int len;
        for (len = 0; isAsciiAlphaOrAbove() && !isWhiteSpaceChar(); next(), len++) {
            if (len < prefixSize) prefix[len] = asciiAlphaToLower(ch_);
        }
        for (int i = len; i < prefixSize; i++) prefix[i] = 0;
        return len;
    }

    bool skip(uint32_t c) {
        if (ch_ == c) {
            next();
            return true;
        }
        return false;
    }

    bool skipWhiteSpace() {
        if (isWhiteSpaceOrLineTerminatorCp(ch_)) {
            next();
            return true;
        }
        return false;
    }

    bool skipParentheses() {
        if (ch_ != '(') return false;
        int balance = 0;
        do {
            if (ch_ == ')') {
                --balance;
            } else if (ch_ == '(') {
                ++balance;
            }
            next();
        } while (balance > 0 && ch_);
        return true;
    }

    bool isEnd() const { return ch_ == 0; }
    bool isAsciiDigit() const { return isAsciiDigitCp(ch_); }
    // Every code point from 'A' up is a word character unless it is a space:
    // that is what makes "é" and U+2028 garbage words rather than separators.
    bool isAsciiAlphaOrAbove() const { return ch_ >= 'A'; }
    bool isWhiteSpaceChar() const { return isWhiteSpaceCp(ch_); }

private:
    std::string_view s_;
    size_t i_ = 0;
    int count_ = 0;
    uint32_t ch_ = 0;
};

enum class KeywordType { Invalid, MonthName, TimeZoneName, TimeSeparator, AmPm };

struct Token {
    enum class Tag { Invalid, Unknown, WhiteSpace, Number, Symbol, EndOfInput, Keyword };

    Tag tag = Tag::Invalid;
    int length = 0;
    int value = -1;
    KeywordType keyword = KeywordType::Invalid;

    bool isInvalid() const { return tag == Tag::Invalid; }
    bool isNumber() const { return tag == Tag::Number; }
    bool isSymbol() const { return tag == Tag::Symbol; }
    bool isWhiteSpace() const { return tag == Tag::WhiteSpace; }
    bool isEndOfInput() const { return tag == Tag::EndOfInput; }
    bool isKeyword() const { return tag == Tag::Keyword; }
    bool isSymbol(char symbol) const { return isSymbol() && value == symbol; }
    bool isKeywordType(KeywordType type) const { return isKeyword() && keyword == type; }
    bool isFixedLengthNumber(int n) const { return isNumber() && length == n; }
    bool isAsciiSign() const { return isSymbol() && (value == '-' || value == '+'); }
    // '+' is 43 and '-' is 45, so this is +1 and -1.
    int asciiSign() const { return 44 - value; }
    bool isKeywordZ() const { return isKeywordType(KeywordType::TimeZoneName) && length == 1 && value == 0; }

    static Token number(int value, int length) { return {Tag::Number, length, value, KeywordType::Invalid}; }
    static Token symbol(char symbol) { return {Tag::Symbol, 1, symbol, KeywordType::Invalid}; }
    static Token keywordOf(KeywordType type, int value, int length) { return {Tag::Keyword, length, value, type}; }
    static Token endOfInput() { return {Tag::EndOfInput, 0, -1, KeywordType::Invalid}; }
    static Token whiteSpace(int length) { return {Tag::WhiteSpace, length, -1, KeywordType::Invalid}; }
    static Token unknown() { return {Tag::Unknown, 1, -1, KeywordType::Invalid}; }
    static Token invalid() { return {Tag::Invalid, 0, -1, KeywordType::Invalid}; }
};

// V8's KeywordTable: a word is classified by its first three letters, lowered.
// A word longer than three letters matches only a month ("January", but also
// "mayonnaise"); "utcc" and "gmtt" are garbage.
struct KeywordEntry {
    char prefix[3];
    KeywordType type;
    int value;
};

const KeywordEntry kKeywords[] = {
    {{'j', 'a', 'n'}, KeywordType::MonthName, 1},
    {{'f', 'e', 'b'}, KeywordType::MonthName, 2},
    {{'m', 'a', 'r'}, KeywordType::MonthName, 3},
    {{'a', 'p', 'r'}, KeywordType::MonthName, 4},
    {{'m', 'a', 'y'}, KeywordType::MonthName, 5},
    {{'j', 'u', 'n'}, KeywordType::MonthName, 6},
    {{'j', 'u', 'l'}, KeywordType::MonthName, 7},
    {{'a', 'u', 'g'}, KeywordType::MonthName, 8},
    {{'s', 'e', 'p'}, KeywordType::MonthName, 9},
    {{'o', 'c', 't'}, KeywordType::MonthName, 10},
    {{'n', 'o', 'v'}, KeywordType::MonthName, 11},
    {{'d', 'e', 'c'}, KeywordType::MonthName, 12},
    {{'a', 'm', '\0'}, KeywordType::AmPm, 0},
    {{'p', 'm', '\0'}, KeywordType::AmPm, 12},
    {{'u', 't', '\0'}, KeywordType::TimeZoneName, 0},
    {{'u', 't', 'c'}, KeywordType::TimeZoneName, 0},
    {{'z', '\0', '\0'}, KeywordType::TimeZoneName, 0},
    {{'g', 'm', 't'}, KeywordType::TimeZoneName, 0},
    {{'c', 'd', 't'}, KeywordType::TimeZoneName, -5},
    {{'c', 's', 't'}, KeywordType::TimeZoneName, -6},
    {{'e', 'd', 't'}, KeywordType::TimeZoneName, -4},
    {{'e', 's', 't'}, KeywordType::TimeZoneName, -5},
    {{'m', 'd', 't'}, KeywordType::TimeZoneName, -6},
    {{'m', 's', 't'}, KeywordType::TimeZoneName, -7},
    {{'p', 'd', 't'}, KeywordType::TimeZoneName, -7},
    {{'p', 's', 't'}, KeywordType::TimeZoneName, -8},
    {{'t', '\0', '\0'}, KeywordType::TimeSeparator, 0},
};

constexpr int kPrefixLength = 3;

const KeywordEntry* lookupKeyword(const uint32_t* pre, int len) {
    for (const KeywordEntry& entry : kKeywords) {
        int j = 0;
        while (j < kPrefixLength && pre[j] == static_cast<uint32_t>(static_cast<unsigned char>(entry.prefix[j]))) {
            j++;
        }
        if (j == kPrefixLength && (len <= kPrefixLength || entry.type == KeywordType::MonthName)) {
            return &entry;
        }
    }
    return nullptr;
}

class Tokenizer {
public:
    explicit Tokenizer(Input* in) : in_(in), next_(scan()) {}

    Token next() {
        const Token result = next_;
        next_ = scan();
        return result;
    }
    Token peek() const { return next_; }
    bool skipSymbol(char symbol) {
        if (next_.isSymbol(symbol)) {
            next_ = scan();
            return true;
        }
        return false;
    }

private:
    Token scan() {
        const int prePos = in_->position();
        if (in_->isEnd()) return Token::endOfInput();
        if (in_->isAsciiDigit()) {
            const int n = in_->readUnsignedNumeral();
            const int length = in_->position() - prePos;
            return Token::number(n, length);
        }
        if (in_->skip(':')) return Token::symbol(':');
        if (in_->skip('-')) return Token::symbol('-');
        if (in_->skip('+')) return Token::symbol('+');
        if (in_->skip('.')) return Token::symbol('.');
        if (in_->skip(')')) return Token::symbol(')');
        if (in_->isAsciiAlphaOrAbove() && !in_->isWhiteSpaceChar()) {
            uint32_t buffer[kPrefixLength] = {0, 0, 0};
            const int length = in_->readWord(buffer, kPrefixLength);
            const KeywordEntry* entry = lookupKeyword(buffer, length);
            if (entry == nullptr) return Token::keywordOf(KeywordType::Invalid, 0, length);
            return Token::keywordOf(entry->type, entry->value, length);
        }
        if (in_->skipWhiteSpace()) {
            return Token::whiteSpace(in_->position() - prePos);
        }
        if (in_->skipParentheses()) {
            return Token::unknown();
        }
        in_->next();
        return Token::unknown();
    }

    Input* in_;
    Token next_;
};

// The first three significant digits of a numeral read as a fraction of a
// second, inferred from the value and the digit count: "5" is 500ms, "05" is
// 50ms, "1234567890" is 123ms.
int readMilliseconds(Token token) {
    int number = token.value;
    int length = token.length;
    if (length < 3) {
        if (length == 1) {
            number *= 100;
        } else if (length == 2) {
            number *= 10;
        }
    } else if (length > 3) {
        if (length > kMaxSignificantDigits) length = kMaxSignificantDigits;
        int factor = 1;
        do {
            factor *= 10;
            length--;
        } while (length > 3);
        number /= factor;
    }
    return number;
}

struct DateFields {
    int year = 0;
    int month = 0;  // 0-based, as MakeDay takes it
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int millisecond = 0;
    bool hasOffset = false;
    int offsetSeconds = 0;
};

class TimeComposer {
public:
    bool isEmpty() const { return index_ == 0; }
    bool isExpecting(int n) const {
        return (index_ == 1 && isMinute(n)) || (index_ == 2 && isSecond(n)) ||
               (index_ == 3 && isMillisecond(n));
    }
    bool add(int n) {
        if (index_ >= kSize) return false;
        comp_[index_++] = n;
        return true;
    }
    bool addFinal(int n) {
        if (!add(n)) return false;
        while (index_ < kSize) comp_[index_++] = 0;
        return true;
    }
    void setHourOffset(int n) { hourOffset_ = n; }

    bool write(DateFields& out) {
        while (index_ < kSize) comp_[index_++] = 0;
        int& hour = comp_[0];
        const int minute = comp_[1];
        const int second = comp_[2];
        const int millisecond = comp_[3];
        if (hourOffset_ != kNone) {
            if (!isHour12(hour)) return false;
            hour %= 12;
            hour += hourOffset_;
        }
        if (!isHour(hour) || !isMinute(minute) || !isSecond(second) || !isMillisecond(millisecond)) {
            // A 24th hour is allowed if minutes, seconds and milliseconds are 0.
            if (hour != 24 || minute != 0 || second != 0 || millisecond != 0) return false;
        }
        out.hour = hour;
        out.minute = minute;
        out.second = second;
        out.millisecond = millisecond;
        return true;
    }

    static bool isMinute(int x) { return between(x, 0, 59); }
    static bool isHour(int x) { return between(x, 0, 23); }
    static bool isSecond(int x) { return between(x, 0, 59); }

private:
    static bool isHour12(int x) { return between(x, 0, 12); }
    static bool isMillisecond(int x) { return between(x, 0, 999); }

    static constexpr int kSize = 4;
    int comp_[kSize] = {0, 0, 0, 0};
    int index_ = 0;
    int hourOffset_ = kNone;
};

class TimeZoneComposer {
public:
    void set(int offsetInHours) {
        sign_ = offsetInHours < 0 ? -1 : 1;
        hour_ = offsetInHours * sign_;
        minute_ = 0;
    }
    void setSign(int sign) { sign_ = sign < 0 ? -1 : 1; }
    void setAbsoluteHour(int hour) { hour_ = hour; }
    void setAbsoluteMinute(int minute) { minute_ = minute; }
    bool isExpecting(int n) const {
        return hour_ != kNone && minute_ == kNone && TimeComposer::isMinute(n);
    }
    bool isUtc() const { return hour_ == 0 && minute_ == 0; }
    bool isEmpty() const { return hour_ == kNone; }

    bool write(DateFields& out) {
        if (sign_ == kNone) {
            out.hasOffset = false;
            return true;
        }
        if (hour_ == kNone) hour_ = 0;
        if (minute_ == kNone) minute_ = 0;
        // Unsigned so that "GMT+999999999:" cannot overflow on the way to
        // being refused; the bound is V8's Smi range on a 64-bit node.
        const unsigned totalSeconds = static_cast<unsigned>(hour_) * 3600U + static_cast<unsigned>(minute_) * 60U;
        if (totalSeconds > static_cast<unsigned>(INT_MAX)) return false;
        out.hasOffset = true;
        out.offsetSeconds = sign_ < 0 ? -static_cast<int>(totalSeconds) : static_cast<int>(totalSeconds);
        return true;
    }

private:
    int sign_ = kNone;
    int hour_ = kNone;
    int minute_ = kNone;
};

class DayComposer {
public:
    bool isEmpty() const { return index_ == 0; }
    bool add(int n) {
        if (index_ >= kSize) return false;
        comp_[index_++] = n;
        return true;
    }
    void setNamedMonth(int n) { namedMonth_ = n; }
    void setIsoDate() { isIsoDate_ = true; }
    static bool isMonth(int x) { return between(x, 1, 12); }
    static bool isDay(int x) { return between(x, 1, 31); }

    bool write(DateFields& out) {
        if (index_ < 1) return false;
        // Day and month default to 1.
        while (index_ < kSize) comp_[index_++] = 1;

        int year = 0;  // Default year is 0 (=> 2000) for KJS compatibility.
        int month = kNone;
        int day = kNone;
        if (namedMonth_ == kNone) {
            if (isIsoDate_ || (index_ == 3 && !isDay(comp_[0]))) {
                // YMD
                year = comp_[0];
                month = comp_[1];
                day = comp_[2];
            } else {
                // MD(Y)
                month = comp_[0];
                day = comp_[1];
                if (index_ == 3) year = comp_[2];
            }
        } else {
            month = namedMonth_;
            if (index_ == 1) {
                // MD or DM
                day = comp_[0];
            } else if (!isDay(comp_[0])) {
                // YMD, MYD, or YDM
                year = comp_[0];
                day = comp_[1];
            } else {
                // DMY, MDY, or DYM
                day = comp_[0];
                year = comp_[1];
            }
        }
        if (!isIsoDate_) {
            if (between(year, 0, 49)) {
                year += 2000;
            } else if (between(year, 50, 99)) {
                year += 1900;
            }
        }
        // No month-length check: "Feb 31" is a legal day component and
        // MakeDay carries it into March, exactly as `new Date(2020, 1, 31)` does.
        if (!isMonth(month) || !isDay(day)) return false;
        out.year = year;
        out.month = month - 1;
        out.day = day;
        return true;
    }

private:
    static constexpr int kSize = 3;
    int comp_[kSize] = {0, 0, 0};
    int index_ = 0;
    int namedMonth_ = kNone;
    bool isIsoDate_ = false;
};

// The ES5 pass. Returns EndOfInput when the whole string was an ES5 date-time
// string, Invalid when it committed to one (read a `T`) and then failed, and
// otherwise the first token it did not consume, which the legacy loop starts
// from with whatever date components were already banked.
Token parseEs5DateTime(Tokenizer& scanner, DayComposer& day, TimeComposer& time, TimeZoneComposer& tz) {
    // Parse mandatory date string: [('-'|'+')yy]yyyy[':'MM[':'DD]]
    if (scanner.peek().isAsciiSign()) {
        // Keep the sign token, so it can be passed back to the legacy parser
        // if it goes unused. NOTE the six-digit number is consumed either way:
        // "-000000" reads as a sign followed by nothing, and so is NaN.
        const Token signToken = scanner.next();
        if (!scanner.peek().isFixedLengthNumber(6)) return signToken;
        const int sign = signToken.asciiSign();
        const int year = scanner.next().value;
        if (sign < 0 && year == 0) return signToken;
        day.add(sign * year);
    } else if (scanner.peek().isFixedLengthNumber(4)) {
        day.add(scanner.next().value);
    } else {
        return scanner.next();
    }
    if (scanner.skipSymbol('-')) {
        if (!scanner.peek().isFixedLengthNumber(2) || !DayComposer::isMonth(scanner.peek().value)) {
            return scanner.next();
        }
        day.add(scanner.next().value);
        if (scanner.skipSymbol('-')) {
            if (!scanner.peek().isFixedLengthNumber(2) || !DayComposer::isDay(scanner.peek().value)) {
                return scanner.next();
            }
            day.add(scanner.next().value);
        }
    }
    // Check for optional time string: 'T'HH':'mm[':'ss['.'sss]]Z
    if (!scanner.peek().isKeywordType(KeywordType::TimeSeparator)) {
        if (!scanner.peek().isEndOfInput()) return scanner.next();
    } else {
        scanner.next();
        if (!scanner.peek().isFixedLengthNumber(2) || !between(scanner.peek().value, 0, 24)) {
            return Token::invalid();
        }
        // Allow 24:00[:00[.000]], but no other time starting with 24.
        const bool hourIs24 = scanner.peek().value == 24;
        time.add(scanner.next().value);
        if (!scanner.skipSymbol(':')) return Token::invalid();
        if (!scanner.peek().isFixedLengthNumber(2) || !TimeComposer::isMinute(scanner.peek().value) ||
            (hourIs24 && scanner.peek().value > 0)) {
            return Token::invalid();
        }
        time.add(scanner.next().value);
        if (scanner.skipSymbol(':')) {
            if (!scanner.peek().isFixedLengthNumber(2) || !TimeComposer::isSecond(scanner.peek().value) ||
                (hourIs24 && scanner.peek().value > 0)) {
                return Token::invalid();
            }
            time.add(scanner.next().value);
            if (scanner.skipSymbol('.')) {
                if (!scanner.peek().isNumber() || (hourIs24 && scanner.peek().value > 0)) {
                    return Token::invalid();
                }
                // Allow more or less than the mandated three digits.
                time.add(readMilliseconds(scanner.next()));
            }
        }
        // Check for optional timezone designation: 'Z' | ('+'|'-')hh':'mm
        if (scanner.peek().isKeywordZ()) {
            scanner.next();
            tz.set(0);
        } else if (scanner.peek().isSymbol('+') || scanner.peek().isSymbol('-')) {
            tz.setSign(scanner.next().value == '+' ? 1 : -1);
            if (scanner.peek().isFixedLengthNumber(4)) {
                // hhmm extension syntax.
                const int hourmin = scanner.next().value;
                const int hour = hourmin / 100;
                const int min = hourmin % 100;
                if (!TimeComposer::isHour(hour) || !TimeComposer::isMinute(min)) return Token::invalid();
                tz.setAbsoluteHour(hour);
                tz.setAbsoluteMinute(min);
            } else {
                // hh:mm standard syntax.
                if (!scanner.peek().isFixedLengthNumber(2) || !TimeComposer::isHour(scanner.peek().value)) {
                    return Token::invalid();
                }
                tz.setAbsoluteHour(scanner.next().value);
                if (!scanner.skipSymbol(':')) return Token::invalid();
                if (!scanner.peek().isFixedLengthNumber(2) || !TimeComposer::isMinute(scanner.peek().value)) {
                    return Token::invalid();
                }
                tz.setAbsoluteMinute(scanner.next().value);
            }
        }
        if (!scanner.peek().isEndOfInput()) return Token::invalid();
    }
    // Successfully parsed an ES5 date-time string. 21.4.1.15: "when the time
    // zone offset is absent, date-only forms are interpreted as a UTC time and
    // date-time forms are interpreted as a local time."
    if (tz.isEmpty() && time.isEmpty()) tz.set(0);
    day.setIsoDate();
    return Token::endOfInput();
}

bool parseFields(std::string_view text, DateFields& out) {
    Input in(text);
    Tokenizer scanner(&in);
    TimeZoneComposer tz;
    TimeComposer time;
    DayComposer day;

    const Token nextUnhandled = parseEs5DateTime(scanner, day, time, tz);
    if (nextUnhandled.isInvalid()) return false;
    bool hasReadNumber = !day.isEmpty();
    // If there's anything left, continue with the legacy parser.
    for (Token token = nextUnhandled; !token.isEndOfInput(); token = scanner.next()) {
        if (token.isNumber()) {
            hasReadNumber = true;
            const int n = token.value;
            if (scanner.skipSymbol(':')) {
                if (scanner.skipSymbol(':')) {
                    // n + "::"
                    if (!time.isEmpty()) return false;
                    time.add(n);
                    time.add(0);
                } else {
                    // n + ":"
                    if (!time.add(n)) return false;
                    if (scanner.peek().isSymbol('.')) scanner.next();
                }
            } else if (scanner.skipSymbol('.') && time.isExpecting(n)) {
                time.add(n);
                if (!scanner.peek().isNumber()) return false;
                const int ms = readMilliseconds(scanner.next());
                if (ms < 0) return false;
                time.addFinal(ms);
            } else if (tz.isExpecting(n)) {
                tz.setAbsoluteMinute(n);
            } else if (time.isExpecting(n)) {
                time.addFinal(n);
                // Require end, white space, "Z", "+" or "-" immediately after
                // finalizing the time.
                const Token peek = scanner.peek();
                if (!peek.isEndOfInput() && !peek.isWhiteSpace() && !peek.isKeywordZ() && !peek.isAsciiSign()) {
                    return false;
                }
            } else {
                if (!day.add(n)) return false;
                scanner.skipSymbol('-');
            }
        } else if (token.isKeyword()) {
            const KeywordType type = token.keyword;
            const int value = token.value;
            if (type == KeywordType::AmPm && !time.isEmpty()) {
                time.setHourOffset(value);
            } else if (type == KeywordType::MonthName) {
                day.setNamedMonth(value);
                scanner.skipSymbol('-');
            } else if (type == KeywordType::TimeZoneName && hasReadNumber) {
                tz.set(value);
            } else {
                // Garbage words are illegal once a number has been read.
                if (hasReadNumber) return false;
                // The first number has to be separated from garbage words by
                // whitespace or other separators.
                if (scanner.peek().isNumber()) return false;
            }
        } else if (token.isAsciiSign() && (tz.isUtc() || !time.isEmpty())) {
            // A UTC offset, only after UTC or a time.
            tz.setSign(token.asciiSign());
            // The following number may be empty.
            int n = 0;
            int length = 0;
            if (scanner.peek().isNumber()) {
                const Token nextToken = scanner.next();
                length = nextToken.length;
                n = nextToken.value;
            }
            hasReadNumber = true;
            if (scanner.peek().isSymbol(':')) {
                tz.setAbsoluteHour(n);
                tz.setAbsoluteMinute(kNone);
            } else if (length == 2 || length == 1) {
                // Time zones like GMT-8.
                tz.setAbsoluteHour(n);
                tz.setAbsoluteMinute(0);
            } else if (length == 4 || length == 3) {
                // The hhmm form.
                tz.setAbsoluteHour(n / 100);
                tz.setAbsoluteMinute(n % 100);
            } else {
                // No need to accept time zones like GMT-12345.
                return false;
            }
        } else if ((token.isAsciiSign() || token.isSymbol(')')) && hasReadNumber) {
            // An extra sign or ')' is illegal once a number has been read.
            return false;
        } else {
            // Ignore other characters and whitespace.
        }
    }
    return day.write(out) && time.write(out) && tz.write(out);
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

std::string localeDateString(double tv) {
    if (std::isnan(tv)) return "Invalid Date";
    const double t = localTime(tv);
    const int m = static_cast<int>(monthFromTime(t)) + 1;
    const int d = static_cast<int>(dateFromTime(t));
    const int y = static_cast<int>(yearFromTime(t));
    return std::to_string(m) + "/" + std::to_string(d) + "/" + std::to_string(y);
}

std::string localeTimeString(double tv) {
    if (std::isnan(tv)) return "Invalid Date";
    const double t = localTime(tv);
    int h = static_cast<int>(hourFromTime(t));
    const char* ampm = (h >= 12) ? "PM" : "AM";
    h = h % 12;
    if (h == 0) h = 12;
    const int m = static_cast<int>(minFromTime(t));
    const int s = static_cast<int>(secFromTime(t));
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%02d:%02d %s", h, m, s, ampm);
    return std::string(buf);
}

std::string localeDateTimeString(double tv) {
    if (std::isnan(tv)) return "Invalid Date";
    return localeDateString(tv) + ", " + localeTimeString(tv);
}

std::string inspectString(double tv) {
    std::string out;
    if (!isoString(tv, out)) return "Invalid Date";
    return out;
}

double parse(std::string_view text) {
    // No trimming: surrounding whitespace is a token like any other, and it
    // matters — " 2020-01-01" is not an ES5 string, so the legacy loop reads
    // it, and a legacy date with no zone is LOCAL where the ES5 date-only
    // form is UTC. That is V8's behaviour and a program that depends on it,
    // knowingly or not, gets the same answer here.
    DateFields fields;
    if (!parseFields(text, fields)) return std::nan("");
    const double dayValue = makeDay(fields.year, fields.month, fields.day);
    const double timeValue = makeTime(fields.hour, fields.minute, fields.second, fields.millisecond);
    double date = makeDate(dayValue, timeValue);
    if (fields.hasOffset) {
        date -= static_cast<double>(fields.offsetSeconds) * kMsPerSecond;
    } else {
        // Local time. V8 bounds the value it will convert to ten days either
        // side of the time-value range, and answers NaN beyond that rather
        // than asking the zone database about a date it cannot represent.
        constexpr double kMaxTimeBeforeUtc = kMaxTimeValue + 10.0 * kMsPerDay;
        if (!(date >= -kMaxTimeBeforeUtc && date <= kMaxTimeBeforeUtc)) return std::nan("");
        date = utcFromLocal(date);
    }
    return timeClip(date);
}

}  // namespace bronze::runtime::datetime
