// String literals, template literals, and the object and array literal
// forms. What these have in common is that each turns source TEXT into a
// value, which is why the escape decoder lives here with them.

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
ExprPtr Parser::parseObjectLit() {
    const Token& openToken = advance();  // '{'
    auto obj = std::make_unique<ObjectLit>();
    obj->span.begin = openToken.span.begin;

    while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile) && !diags_.hasErrors()) {
        std::string keyStr;
        if (check(TokenKind::Identifier)) {
            keyStr = std::string(advance().text);
        } else if (check(TokenKind::StringLiteral)) {
            auto sTok = advance();
            keyStr = decodeStringLiteral(sTok.text.substr(1, sTok.text.size() - 2), sTok.span);
        } else if (check(TokenKind::Ellipsis)) {
            // `{ ...src }` - a spread in key position, which the property
            // key error named as a missing identifier.
            error("unsupported construct: spread");
            return nullptr;
        } else {
            error("expected identifier or string literal for property key");
            return nullptr;
        }

        if (!expect(TokenKind::Colon, "':' after property key")) return nullptr;

        auto valExpr = parseExpr();
        if (!valExpr) return nullptr;

        obj->props.push_back(ObjectProp{std::move(keyStr), std::move(valExpr)});

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
        auto elemExpr = parseExpr();
        if (!elemExpr) return nullptr;
        arr->elements.push_back(std::move(elemExpr));
        if (!match(TokenKind::Comma)) break;
    }

    if (!expect(TokenKind::RBracket, "']' after array literal")) return nullptr;
    arr->span.end = peek().span.begin;
    return arr;
}

}  // namespace bronze
