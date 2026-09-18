// `RegExp.escape` (ECMA-262 22.2.5.2): the one member of the RegExp
// constructor that is an algorithm of its own — a code-point walk that decides,
// per character, which of three escape spellings keeps it inert inside a
// pattern. Its own translation unit because nothing else in the RegExp surface
// shares a line with it: it reads no pattern, no flags and no `lastIndex`.

#include <cstring>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/builtin_regexp_internal.h"
#include "runtime/exception.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// SyntaxCharacter :: one of ^ $ \ . * + ? ( ) [ ] { } | — the characters that
// mean something to the pattern grammar and so must be backslashed to mean
// themselves.
bool isSyntaxCharacter(uint32_t cp) {
    switch (cp) {
        case '^': case '$': case '\\': case '.': case '*': case '+': case '?':
        case '(': case ')': case '[': case ']': case '{': case '}': case '|':
            return true;
        default:
            return false;
    }
}

void appendHex(std::string& out, uint32_t value, size_t width) {
    static const char kDigits[] = "0123456789abcdef";
    std::string digits;
    do {
        digits.insert(digits.begin(), kDigits[value & 0xF]);
        value >>= 4;
    } while (value != 0);
    while (digits.size() < width) digits.insert(digits.begin(), '0');
    out += digits;
}

// 22.2.5.2.1 EncodeForRegExpEscape. Three tiers: a syntax character (and `/`,
// which a LITERAL would otherwise end) takes a backslash; the five control
// characters take their named escape; and anything a reader could mistake for
// punctuation, whitespace, or a lone surrogate takes a numeric escape.
//
// The third tier is what makes the member worth having over a hand-rolled
// `replace`: `escape("a b")` is "a\x20b", so the result stays safe to paste
// into an `x`-flagged pattern or a string built by concatenation.
void encodeForRegExpEscape(std::string& out, uint32_t cp) {
    if (isSyntaxCharacter(cp) || cp == '/') {
        out.push_back('\\');
        // Every syntax character is ASCII, so one byte is the whole encoding.
        out.push_back(static_cast<char>(cp));
        return;
    }
    switch (cp) {
        case 0x09: out += "\\t"; return;
        case 0x0A: out += "\\n"; return;
        case 0x0B: out += "\\v"; return;
        case 0x0C: out += "\\f"; return;
        case 0x0D: out += "\\r"; return;
        default: break;
    }
    // Step 3's otherPunctuators, plus WhiteSpace, plus LineTerminator, plus a
    // lone surrogate — a code point that must not be left bare.
    static const char* const kOtherPunctuators = ",-=<>#&!%:;@~'`\"";
    const bool punctuator = cp < 0x80 && std::strchr(kOtherPunctuators, static_cast<char>(cp)) &&
                            cp != 0;
    const bool space = cp == 0x20 || cp == 0xA0 || cp == 0x1680 ||
                       (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F ||
                       cp == 0x3000 || cp == 0xFEFF;
    const bool lineTerminator = cp == 0x2028 || cp == 0x2029;
    const bool surrogate = cp >= 0xD800 && cp <= 0xDFFF;
    if (punctuator || space || lineTerminator || surrogate) {
        if (cp <= 0xFF) {
            out += "\\x";
            appendHex(out, cp, 2);
            return;
        }
        out += "\\u{";
        appendHex(out, cp, 1);
        out += "}";
        return;
    }
    // Anything else is itself. Written back as UTF-8, which is the encoding
    // `rtMakeString` reads.
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

}  // namespace

// 22.2.5.2 RegExp.escape(S). A non-string argument is a TypeError and NOT
// ToString'd (step 1), which is the member deliberately refusing to guess: the
// whole point of it is that the result is safe to interpolate, and a number
// silently stringified is how a caller ends up escaping the wrong thing.
uint64_t rtRegExpEscapeBody(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!args[0].isString()) {
        return rtThrowTypeError("RegExp.escape requires a string argument").rawBits();
    }
    Rooted<Value> input{args[0]};
    const std::vector<uint16_t> units = rtStringUnits(input.get().asString<StringHeader>());
    std::string out;
    for (size_t i = 0; i < units.size(); ++i) {
        uint32_t cp = units[i];
        // StringToCodePoints: a well-formed pair is ONE code point, which is
        // what keeps an astral character out of the lone-surrogate escape above.
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < units.size() && units[i + 1] >= 0xDC00 &&
            units[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (units[i + 1] - 0xDC00);
            ++i;
        }
        // Step 3.a: the FIRST code point takes a hex escape when it is a digit
        // or an ASCII letter, so the result can never start a flag or read as
        // part of an identifier at the splice point. Only the first — `escape`
        // of "ab" is "\x61b".
        const bool first = out.empty();
        const bool alnum = (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') ||
                           (cp >= 'A' && cp <= 'Z');
        if (first && alnum) {
            out += "\\x";
            appendHex(out, cp, 2);
            continue;
        }
        encodeForRegExpEscape(out, cp);
    }
    return rtMakeString(out).rawBits();
}

}  // namespace bronze::runtime
