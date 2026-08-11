#include "json/json.h"

#include <charconv>
#include <cstdlib>
#include <string>

namespace bronze::json {

namespace {

// Recursive descent, like every other parser in this project, over a grammar
// small enough to read in one screen (RFC 8259 / ECMA-262 25.5.1):
//
//   JSONValue  := JSONNullLiteral | JSONBooleanLiteral | JSONString
//               | JSONNumber | JSONObject | JSONArray
//   JSONObject := '{' '}' | '{' Member (',' Member)* '}'
//   JSONArray  := '[' ']' | '[' JSONValue (',' JSONValue)* ']'
//
// The productions with no comma before the closing bracket are written out
// rather than folded into a "loop until close" — that fold is exactly how a
// trailing comma becomes accepted, and rejecting it is half the point of this
// file existing.
class Parser {
public:
    Parser(UnitsView text, std::string& error) : text_(text), error_(error) {}

    ValuePtr parseText() {
        skipWhitespace();
        ValuePtr value = parseValue();
        if (!value) return nullptr;
        skipWhitespace();
        if (pos_ != text_.size()) {
            fail("unexpected text after the end of the JSON value");
            return nullptr;
        }
        return value;
    }

private:
    // A depth bound, because a recursive-descent parser over attacker-shaped
    // input is otherwise a stack overflow waiting for `[[[[[[...`. Named
    // rather than crashed.
    static constexpr uint32_t kMaxDepth = 512;

    UnitsView text_;
    std::string& error_;
    size_t pos_ = 0;
    uint32_t depth_ = 0;

    bool atEnd() const { return pos_ >= text_.size(); }
    char16_t peek() const { return pos_ < text_.size() ? text_[pos_] : u'\0'; }

    void fail(const std::string& what) {
        if (!error_.empty()) return;  // the first error is the useful one
        error_ = "Unexpected token in JSON at position " + std::to_string(pos_) + ": " + what;
    }

    // 25.5.1's JSONWhitespace is exactly these four. A JS comment, a vertical
    // tab and U+00A0 are all whitespace to the JavaScript lexer and none of
    // them is whitespace here.
    void skipWhitespace() {
        while (!atEnd()) {
            const char16_t c = text_[pos_];
            if (c == u' ' || c == u'\t' || c == u'\n' || c == u'\r') {
                ++pos_;
                continue;
            }
            return;
        }
    }

    bool matchWord(const char* word) {
        const size_t len = std::char_traits<char>::length(word);
        if (text_.size() - pos_ < len) return false;
        for (size_t i = 0; i < len; ++i) {
            if (text_[pos_ + i] != static_cast<char16_t>(word[i])) return false;
        }
        pos_ += len;
        return true;
    }

    ValuePtr parseValue() {
        if (atEnd()) {
            fail("a value was expected");
            return nullptr;
        }
        const char16_t c = peek();
        switch (c) {
            case u'{': return parseObject();
            case u'[': return parseArray();
            case u'"': return parseStringValue();
            case u't': {
                if (!matchWord("true")) return unexpected();
                auto v = std::make_unique<Value>();
                v->kind = Value::Kind::Bool;
                v->boolean = true;
                return v;
            }
            case u'f': {
                if (!matchWord("false")) return unexpected();
                auto v = std::make_unique<Value>();
                v->kind = Value::Kind::Bool;
                v->boolean = false;
                return v;
            }
            case u'n': {
                if (!matchWord("null")) return unexpected();
                auto v = std::make_unique<Value>();
                v->kind = Value::Kind::Null;
                return v;
            }
            default: break;
        }
        if (c == u'-' || (c >= u'0' && c <= u'9')) return parseNumber();
        return unexpected();
    }

    ValuePtr unexpected() {
        const char16_t c = peek();
        std::string what = "a JSON value was expected";
        if (c == u'\'') what = "a string must be double-quoted";
        else if (c == u'+' || c == u'.') what = "a JSON number has no leading '+' or '.'";
        else if (c == u'/') what = "JSON has no comments";
        else if (c == u']' || c == u'}') what = "a trailing comma is not a JSON value";
        fail(what);
        return nullptr;
    }

    struct DepthGuard {
        Parser& p;
        bool ok;
        explicit DepthGuard(Parser& parser) : p(parser), ok(++parser.depth_ <= kMaxDepth) {
            if (!ok) p.fail("the JSON value nests too deeply");
        }
        ~DepthGuard() { --p.depth_; }
    };

    ValuePtr parseObject() {
        DepthGuard guard(*this);
        if (!guard.ok) return nullptr;
        ++pos_;  // '{'
        auto obj = std::make_unique<Value>();
        obj->kind = Value::Kind::Object;
        skipWhitespace();
        if (peek() == u'}') {
            ++pos_;
            return obj;
        }
        for (;;) {
            skipWhitespace();
            if (peek() != u'"') {
                // An unquoted key is the single most common JS-only spelling,
                // so it is named rather than reported as "unexpected token".
                fail("an object key must be a double-quoted string");
                return nullptr;
            }
            Units key;
            if (!parseString(key)) return nullptr;
            skipWhitespace();
            if (peek() != u':') {
                fail("':' was expected after an object key");
                return nullptr;
            }
            ++pos_;
            skipWhitespace();
            ValuePtr value = parseValue();
            if (!value) return nullptr;
            obj->members.push_back(Member{std::move(key), std::move(value)});
            skipWhitespace();
            if (peek() == u',') {
                ++pos_;
                continue;  // and the next iteration demands a KEY, not a '}'
            }
            if (peek() == u'}') {
                ++pos_;
                return obj;
            }
            fail("',' or '}' was expected after an object member");
            return nullptr;
        }
    }

    ValuePtr parseArray() {
        DepthGuard guard(*this);
        if (!guard.ok) return nullptr;
        ++pos_;  // '['
        auto arr = std::make_unique<Value>();
        arr->kind = Value::Kind::Array;
        skipWhitespace();
        if (peek() == u']') {
            ++pos_;
            return arr;
        }
        for (;;) {
            skipWhitespace();
            ValuePtr element = parseValue();
            if (!element) return nullptr;
            arr->elements.push_back(std::move(element));
            skipWhitespace();
            if (peek() == u',') {
                ++pos_;
                continue;  // and the next iteration demands a VALUE, not a ']'
            }
            if (peek() == u']') {
                ++pos_;
                return arr;
            }
            fail("',' or ']' was expected after an array element");
            return nullptr;
        }
    }

    ValuePtr parseStringValue() {
        auto v = std::make_unique<Value>();
        v->kind = Value::Kind::String;
        if (!parseString(v->text)) return nullptr;
        return v;
    }

    bool hexDigit(char16_t c, uint32_t& out) {
        if (c >= u'0' && c <= u'9') out = static_cast<uint32_t>(c - u'0');
        else if (c >= u'a' && c <= u'f') out = static_cast<uint32_t>(c - u'a') + 10;
        else if (c >= u'A' && c <= u'F') out = static_cast<uint32_t>(c - u'A') + 10;
        else return false;
        return true;
    }

    // JSONString, escape by escape. `\u` produces ONE code unit and is not
    // paired with a following surrogate here: a JSON string is a sequence of
    // code units and `"\ud800"` is a legal one, so pairing would be inventing
    // a value the text did not contain.
    bool parseString(Units& out) {
        ++pos_;  // opening quote
        for (;;) {
            if (atEnd()) {
                fail("the string was not closed before the end of the input");
                return false;
            }
            const char16_t c = text_[pos_];
            if (c == u'"') {
                ++pos_;
                return true;
            }
            if (c < 0x20) {
                // 25.5.1: a raw control character is not a JSONStringCharacter,
                // which is why an embedded newline must be written "\n".
                fail("a control character must be escaped inside a JSON string");
                return false;
            }
            if (c != u'\\') {
                out.push_back(c);
                ++pos_;
                continue;
            }
            ++pos_;
            if (atEnd()) {
                fail("the string ended inside an escape");
                return false;
            }
            const char16_t e = text_[pos_++];
            switch (e) {
                case u'"': out.push_back(u'"'); break;
                case u'\\': out.push_back(u'\\'); break;
                case u'/': out.push_back(u'/'); break;
                case u'b': out.push_back(u'\b'); break;
                case u'f': out.push_back(u'\f'); break;
                case u'n': out.push_back(u'\n'); break;
                case u'r': out.push_back(u'\r'); break;
                case u't': out.push_back(u'\t'); break;
                case u'u': {
                    if (text_.size() - pos_ < 4) {
                        fail("\\u needs four hexadecimal digits");
                        return false;
                    }
                    uint32_t value = 0;
                    for (int i = 0; i < 4; ++i) {
                        uint32_t digit = 0;
                        if (!hexDigit(text_[pos_ + static_cast<size_t>(i)], digit)) {
                            fail("\\u needs four hexadecimal digits");
                            return false;
                        }
                        value = value * 16 + digit;
                    }
                    pos_ += 4;
                    out.push_back(static_cast<char16_t>(value));
                    break;
                }
                default:
                    // `\x41`, `\'`, `\0` and a line continuation are all
                    // JavaScript string escapes and none of them is JSON.
                    fail("that escape is not one JSON defines");
                    return false;
            }
        }
    }

    // JSONNumber: an optional '-', an integer part with no leading zero, an
    // optional fraction with at least one digit, an optional exponent with at
    // least one digit. Every one of those "at least one" clauses is a place
    // JavaScript is more permissive, so each is checked rather than left to
    // strtod, which would happily accept `1.` and `.5`.
    ValuePtr parseNumber() {
        const size_t start = pos_;
        if (peek() == u'-') ++pos_;
        if (atEnd() || peek() < u'0' || peek() > u'9') {
            fail("a digit was expected in a JSON number");
            return nullptr;
        }
        if (peek() == u'0') {
            ++pos_;
            if (!atEnd() && peek() >= u'0' && peek() <= u'9') {
                fail("a JSON number has no leading zero");
                return nullptr;
            }
        } else {
            while (!atEnd() && peek() >= u'0' && peek() <= u'9') ++pos_;
        }
        if (peek() == u'.') {
            ++pos_;
            if (atEnd() || peek() < u'0' || peek() > u'9') {
                fail("a digit was expected after the decimal point");
                return nullptr;
            }
            while (!atEnd() && peek() >= u'0' && peek() <= u'9') ++pos_;
        }
        if (peek() == u'e' || peek() == u'E') {
            ++pos_;
            if (peek() == u'+' || peek() == u'-') ++pos_;
            if (atEnd() || peek() < u'0' || peek() > u'9') {
                fail("a digit was expected in the exponent");
                return nullptr;
            }
            while (!atEnd() && peek() >= u'0' && peek() <= u'9') ++pos_;
        }

        // The grammar above admits only ASCII, so narrowing the accepted span
        // loses nothing — and the VALUE is then ordinary strtod, because
        // 25.5.1 defines it as MV of the StrNumericLiteral, which is the same
        // correctly-rounded decimal-to-double conversion.
        std::string ascii;
        ascii.reserve(pos_ - start);
        for (size_t i = start; i < pos_; ++i) ascii.push_back(static_cast<char>(text_[i]));
        auto v = std::make_unique<Value>();
        v->kind = Value::Kind::Number;
        v->number = std::strtod(ascii.c_str(), nullptr);
        return v;
    }
};

}  // namespace

ValuePtr parse(UnitsView text, std::string& error) {
    error.clear();
    Parser parser(text, error);
    ValuePtr value = parser.parseText();
    if (!value && error.empty()) error = "Unexpected end of JSON input";
    return value;
}

}  // namespace bronze::json
