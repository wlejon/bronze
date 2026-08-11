// String literals, numeric literals, template literals, and the object and
// array literal forms. What these have in common is that each turns source
// TEXT into a value, which is why the escape decoder and the numeric decoder
// live here with them.

#include <charconv>
#include <string>

#include "parse/parser.h"

namespace bronze {

using namespace ast;

namespace {

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
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

bool readHex(std::string_view text, size_t at, size_t count, uint32_t& out) {
    if (at + count > text.size()) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < count; ++i) {
        char c = text[at + i];
        uint32_t digit;
        if (c >= '0' && c <= '9') digit = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = static_cast<uint32_t>(c - 'A' + 10);
        else return false;
        value = value * 16 + digit;
    }
    out = value;
    return true;
}

bool isDecimalDigit(char c) { return c >= '0' && c <= '9'; }

// The value of a digit in base 36, which is enough to answer for every radix
// a numeric literal can have and to REJECT a digit the radix does not have —
// `0b19` must name the offending digit rather than stop reading at the 1.
int digitValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

const char* radixName(int radix) {
    switch (radix) {
        case 2: return "binary";
        case 8: return "octal";
        case 16: return "hexadecimal";
        default: return "decimal";
    }
}

}  // namespace
// The characters a string literal DENOTES, which is not the source text
// between the quotes: `"a\nb"` is three characters, and reading it as four
// is a wrong answer given quietly — `"\n".length` was 2, and a string
// carrying a literal backslash-n went on to be printed, compared and
// concatenated as if that were what the program said.
//
// The lexer deliberately does not do this: it only has to find the end of
// the literal, and a token that carried a decoded value would make the
// source span and the token text disagree about what they describe.
std::string Parser::decodeStringLiteral(std::string_view raw, Span span) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '\\') {
            out.push_back(raw[i]);
            continue;
        }
        if (++i >= raw.size()) break;  // the lexer rejects a trailing backslash
        switch (raw[i]) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'v': out.push_back('\v'); break;
            // \0 is the NUL character, but only when no digit follows it:
            // `\01` is a legacy octal escape, which strict-mode JS rejects
            // outright rather than guessing at.
            case '0':
                if (i + 1 < raw.size() && raw[i + 1] >= '0' && raw[i + 1] <= '9') {
                    diags_.error(span, "unsupported construct: legacy octal escape in a string");
                    return out;
                }
                out.push_back('\0');
                break;
            case '\n': break;  // line continuation: denotes nothing
            case '\r':
                if (i + 1 < raw.size() && raw[i + 1] == '\n') ++i;
                break;
            case 'x': {
                uint32_t cp = 0;
                if (!readHex(raw, i + 1, 2, cp)) {
                    diags_.error(span, "invalid '\\x' escape: two hex digits expected");
                    return out;
                }
                appendUtf8(out, cp);
                i += 2;
                break;
            }
            case 'u': {
                uint32_t cp = 0;
                if (i + 1 < raw.size() && raw[i + 1] == '{') {
                    size_t close = raw.find('}', i + 2);
                    if (close == std::string_view::npos || close == i + 2 ||
                        !readHex(raw, i + 2, close - (i + 2), cp) || cp > 0x10FFFF) {
                        diags_.error(span, "invalid '\\u{...}' escape");
                        return out;
                    }
                    i = close;
                } else {
                    if (!readHex(raw, i + 1, 4, cp)) {
                        diags_.error(span, "invalid '\\u' escape: four hex digits expected");
                        return out;
                    }
                    i += 4;
                    // A surrogate PAIR written as two \u escapes is one code
                    // point; encoding each half separately would produce two
                    // ill-formed UTF-8 sequences instead of the character.
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < raw.size() && raw[i + 1] == '\\' &&
                        raw[i + 2] == 'u') {
                        uint32_t low = 0;
                        if (readHex(raw, i + 3, 4, low) && low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            i += 6;
                        }
                    }
                }
                appendUtf8(out, cp);
                break;
            }
            // Everything else denotes itself, which is the language's rule
            // for an unrecognized escape: `\q` is `q`, and `\\`, `\'`, `\"`
            // and `\/` all fall out of it.
            default: out.push_back(raw[i]); break;
        }
    }
    return out;
}
// The Number a NumericLiteral DENOTES (ECMA-262 12.9.3), which — like a
// string literal's characters — is not its source text. The lexer only found
// where the literal ends; deciding what `0xFF`, `1_000_000` and `1.5e-3` mean
// is this function's job, and keeping it here is what lets the lexer stay
// permissive enough to hand a malformed literal over as ONE token, so the
// diagnostic can point at the whole thing.
//
// Three rules from the grammar, each of which is an error rather than a
// guess:
//
//  - a NumericLiteralSeparator contributes nothing to the mathematical value,
//    but it may appear only BETWEEN two digits, so `1__0`, `1_`, `0x_1` and
//    `1_.5` are not numbers;
//  - a digit the radix does not have (`0b19`) is not a number; and
//  - `017` is a LegacyOctalIntegerLiteral, which strict mode forbids
//    outright. Reading it as decimal 17 or as octal 15 are both defensible
//    and they differ, which is exactly the situation docs/0000 says to
//    diagnose rather than choose.
bool Parser::decodeNumericLiteral(std::string_view raw, Span span, double& out) {
    out = 0;
    if (raw.empty()) {
        diags_.error(span, "empty numeric literal");
        return false;
    }

    int radix = 10;
    size_t digitsBegin = 0;
    if (raw.size() >= 2 && raw[0] == '0') {
        switch (raw[1]) {
            case 'x': case 'X': radix = 16; digitsBegin = 2; break;
            case 'o': case 'O': radix = 8;  digitsBegin = 2; break;
            case 'b': case 'B': radix = 2;  digitsBegin = 2; break;
            default:
                // A leading zero followed by a digit or a separator is a
                // legacy octal (or a NonOctalDecimalIntegerLiteral like `08`);
                // `0.5`, `0e3` and a bare `0` are ordinary decimals.
                if (isDecimalDigit(raw[1]) || raw[1] == '_') {
                    diags_.error(span, "legacy octal literal '" + std::string(raw) +
                                           "': a numeric literal may not start with '0' "
                                           "followed by a digit (write 0o" +
                                           std::string(raw.substr(1)) + " for octal, or " +
                                           std::string(raw.substr(1)) + " for decimal)");
                    return false;
                }
                break;
        }
    }

    // Separator placement is checked against the ORIGINAL text, because the
    // rule is about which characters neighbour it.
    const auto isDigitOfRadix = [&](size_t at) {
        if (at >= raw.size()) return false;
        const int v = digitValue(raw[at]);
        return v >= 0 && v < radix;
    };
    for (size_t i = digitsBegin; i < raw.size(); ++i) {
        if (raw[i] != '_') continue;
        if (i == digitsBegin || !isDigitOfRadix(i - 1) || !isDigitOfRadix(i + 1)) {
            diags_.error(span, "numeric separator '_' must appear between two digits, in '" +
                                   std::string(raw) + "'");
            return false;
        }
    }

    if (radix != 10) {
        double value = 0;
        size_t digits = 0;
        for (size_t i = digitsBegin; i < raw.size(); ++i) {
            if (raw[i] == '_') continue;
            const int v = digitValue(raw[i]);
            if (v < 0 || v >= radix) {
                diags_.error(span, std::string("invalid digit '") + raw[i] + "' in the " +
                                       radixName(radix) + " literal '" + std::string(raw) + "'");
                return false;
            }
            value = value * radix + v;
            ++digits;
        }
        if (digits == 0) {
            diags_.error(span, std::string("the ") + radixName(radix) + " literal '" +
                                   std::string(raw) + "' has no digits after its prefix");
            return false;
        }
        out = value;
        return true;
    }

    // Decimal: the separators are the only thing between this text and
    // something `from_chars` reads. A leading `.` is normalized because the
    // literal `.5` is a DecimalLiteral and `from_chars` is not required to
    // accept one.
    std::string text;
    text.reserve(raw.size() + 1);
    if (raw[0] == '.') text.push_back('0');
    for (char c : raw) {
        if (c != '_') text.push_back(c);
    }

    double value = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        // Out of range is the one `errc` that is not a malformed literal:
        // ECMA-262 rounds an over-large MV to +Infinity and an under-small
        // one to zero, which is what `from_chars` already put in `value`.
        if (result.ec == std::errc::result_out_of_range && result.ptr == end) {
            out = value;
            return true;
        }
        diags_.error(span, "malformed numeric literal '" + std::string(raw) + "'");
        return false;
    }
    out = value;
    return true;
}

// `head ${ expr } middle ${ expr } tail`, with the lexer having already
// decided where each piece ends. The delimiters are stripped by span
// arithmetic: a head is `...${ (backtick plus two), a middle is }...${ and
// a tail is }...` — so every piece drops one leading and two-or-one
// trailing characters, and what is left is decoded like any string literal.
ExprPtr Parser::parseTemplateLiteral() {
    auto lit = std::make_unique<TemplateLit>();
    const Token& headTok = peek();
    lit->span = headTok.span;

    auto cook = [&](const Token& tok, size_t trailing) {
        return decodeStringLiteral(tok.text.substr(1, tok.text.size() - 1 - trailing), tok.span);
    };

    lit->quasis.push_back(cook(advance(), 2));  // strips the `${`
    for (;;) {
        auto expr = parseExpr();
        if (!expr) return nullptr;
        lit->exprs.push_back(std::move(expr));

        if (check(TokenKind::TemplateMiddle)) {
            lit->quasis.push_back(cook(advance(), 2));
            continue;
        }
        if (check(TokenKind::TemplateTail)) {
            const Token& tail = advance();
            lit->quasis.push_back(cook(tail, 1));  // strips the closing backtick
            lit->span = {lit->span.begin, tail.span.end};
            return lit;
        }
        error("expected '}' to close a template substitution");
        return nullptr;
    }
}
// The four PropertyDefinition forms bronze has (ECMA-262 13.2.5): a written
// key with a value, a computed key with a value, and the IdentifierReference
// shorthand. Every value is an *AssignmentExpression*, so the commas between
// properties are the literal's own punctuation and never the comma operator.
ExprPtr Parser::parseObjectLit() {
    const Token& openToken = advance();  // '{'
    auto obj = std::make_unique<ObjectLit>();
    obj->span.begin = openToken.span.begin;

    while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile) && !diags_.hasErrors()) {
        ObjectProp prop;
        if (check(TokenKind::LBracket)) {
            // `{ [e]: v }`. The key is not known here: ToPropertyKey runs on
            // whatever `e` evaluates to, at run time, and BEFORE `v` is
            // evaluated — which is why the key expression is a child of the
            // property rather than something the parser folds to text.
            advance();
            prop.keyExpr = parseAssign();
            if (!prop.keyExpr) return nullptr;
            if (!expect(TokenKind::RBracket, "']' after a computed property key")) return nullptr;
            if (!expect(TokenKind::Colon, "':' after a computed property key")) return nullptr;
            prop.value = parseAssign();
            if (!prop.value) return nullptr;
        } else if (check(TokenKind::Identifier)) {
            const Token& nameTok = advance();
            prop.key = std::string(nameTok.text);
            if (check(TokenKind::LParen)) {
                // `{ m() {} }` — MethodDefinition shorthand. It is a function
                // whose home object is this literal, which is a `super`
                // question and not a property-key one, so it is named rather
                // than reported as a missing ':'.
                error("unsupported construct: object literal method shorthand");
                return nullptr;
            }
            if (check(TokenKind::Assign)) {
                // `{ x = 1 }` is a CoverInitializedName: the cover grammar
                // admits it so that `({ x = 1 } = o)` can parse, and it is
                // never an object literal in its own right. It is parsed as
                // the assignment it looks like and FLAGGED, so the refinement
                // at the `=` can read the default off it and lowering can
                // refuse a literal that reaches it unrefined.
                advance();
                auto target = std::make_unique<Ident>();
                target->span = nameTok.span;
                target->name = prop.key;
                auto init = parseAssign();
                if (!init) return nullptr;
                auto cover = std::make_unique<Binary>();
                cover->span = {nameTok.span.begin, init->span.end};
                cover->op = BinaryOp::Assign;
                cover->lhs = std::move(target);
                cover->rhs = std::move(init);
                prop.coverInitialized = true;
                prop.value = std::move(cover);
                obj->props.push_back(std::move(prop));
                if (!match(TokenKind::Comma)) break;
                continue;
            }
            if (check(TokenKind::Comma) || check(TokenKind::RBrace)) {
                // `{ x }` — shorthand. The key is the identifier's text and
                // the value is that same identifier evaluated here, so the
                // two cannot disagree about which binding is meant.
                auto ident = std::make_unique<Ident>();
                ident->span = nameTok.span;
                ident->name = prop.key;
                prop.value = std::move(ident);
            } else {
                if (!expect(TokenKind::Colon, "':' after property key")) return nullptr;
                prop.value = parseAssign();
                if (!prop.value) return nullptr;
            }
        } else if (check(TokenKind::StringLiteral)) {
            auto sTok = advance();
            prop.key = decodeStringLiteral(sTok.text.substr(1, sTok.text.size() - 2), sTok.span);
            if (!expect(TokenKind::Colon, "':' after property key")) return nullptr;
            prop.value = parseAssign();
            if (!prop.value) return nullptr;
        } else if (check(TokenKind::Ellipsis)) {
            // `{ ...src }` — a property definition with no key of its own: it
            // contributes every own enumerable property of `src`, in the order
            // docs/0009 pins, at the position it is written.
            const Token& dots = advance();
            auto spread = std::make_unique<SpreadElement>();
            spread->argument = parseAssign();
            if (!spread->argument) return nullptr;
            spread->span = {dots.span.begin, spread->argument->span.end};
            prop.value = std::move(spread);
        } else {
            // A numeric key (`{ 1: 'a' }`) lands here deliberately: its name
            // is ToString(Number), which is the runtime's formatter and not
            // something the parser may reimplement. `{ [1]: 'a' }` is the
            // spelling that works, and it is the same property.
            error("expected a property key: an identifier, a string literal, "
                  "or a computed '[expr]'");
            return nullptr;
        }

        obj->props.push_back(std::move(prop));

        if (!match(TokenKind::Comma)) break;
    }

    if (!expect(TokenKind::RBrace, "'}' after object literal")) return nullptr;
    obj->span.end = peek().span.begin;
    return obj;
}

ExprPtr Parser::parseArrayLit() {
    const Token& openToken = advance();  // '['
    auto arr = std::make_unique<ArrayLit>();
    arr->span.begin = openToken.span.begin;

    while (!check(TokenKind::RBracket) && !check(TokenKind::EndOfFile) && !diags_.hasErrors()) {
        if (check(TokenKind::Comma)) {
            // `[1, , 2]` — an ElementList elision, which denotes a HOLE and
            // not `undefined`: the two differ under `in` and under the array
            // methods that skip holes. bronze has no sparse arrays, so this
            // is named rather than quietly filled in.
            error("unsupported construct: an elision (a hole) in an array literal");
            return nullptr;
        }
        // AssignmentExpression, for the same reason the argument list is:
        // a comma operator here would make `[1, 2, 3]` one element long.
        // A `...` element contributes several, so the list parses it.
        if (check(TokenKind::Ellipsis)) {
            const Token& dots = advance();
            auto spread = std::make_unique<SpreadElement>();
            spread->argument = parseAssign();
            if (!spread->argument) return nullptr;
            spread->span = {dots.span.begin, spread->argument->span.end};
            arr->elements.push_back(std::move(spread));
        } else {
            auto elemExpr = parseAssign();
            if (!elemExpr) return nullptr;
            arr->elements.push_back(std::move(elemExpr));
        }
        if (!match(TokenKind::Comma)) break;
    }

    if (!expect(TokenKind::RBracket, "']' after array literal")) return nullptr;
    arr->span.end = peek().span.begin;
    return arr;
}

}  // namespace bronze
