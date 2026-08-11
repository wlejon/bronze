#include "lex/lexer.h"

namespace bronze {

const char* tokenKindName(TokenKind kind) {
    switch (kind) {
        case TokenKind::EndOfFile: return "eof";
        case TokenKind::Identifier: return "ident";
        case TokenKind::NumberLiteral: return "number";
        case TokenKind::StringLiteral: return "string";
        case TokenKind::KwBreak: return "break";
        case TokenKind::KwCase: return "case";
        case TokenKind::KwCatch: return "catch";
        case TokenKind::KwConst: return "const";
        case TokenKind::KwContinue: return "continue";
        case TokenKind::KwDefault: return "default";
        case TokenKind::KwDo: return "do";
        case TokenKind::KwElse: return "else";
        case TokenKind::KwExport: return "export";
        case TokenKind::KwFalse: return "false";
        case TokenKind::KwFor: return "for";
        case TokenKind::KwFunction: return "function";
        case TokenKind::KwIf: return "if";
        case TokenKind::KwImport: return "import";
        case TokenKind::KwIn: return "in";
        case TokenKind::KwLet: return "let";
        case TokenKind::KwNew: return "new";
        case TokenKind::KwNull: return "null";
        case TokenKind::KwOf: return "of";
        case TokenKind::KwReturn: return "return";
        case TokenKind::KwSwitch: return "switch";
        case TokenKind::KwThrow: return "throw";
        case TokenKind::KwTrue: return "true";
        case TokenKind::KwTry: return "try";
        case TokenKind::KwDelete: return "delete";
        case TokenKind::KwThis: return "this";
        case TokenKind::KwUndefined: return "undefined";
        case TokenKind::KwVar: return "var";
        case TokenKind::KwWhile: return "while";
        case TokenKind::LParen: return "(";
        case TokenKind::RParen: return ")";
        case TokenKind::LBrace: return "{";
        case TokenKind::RBrace: return "}";
        case TokenKind::LBracket: return "[";
        case TokenKind::RBracket: return "]";
        case TokenKind::Comma: return ",";
        case TokenKind::Semicolon: return ";";
        case TokenKind::Colon: return ":";
        case TokenKind::Dot: return ".";
        case TokenKind::Arrow: return "=>";
        case TokenKind::Assign: return "=";
        case TokenKind::Plus: return "+";
        case TokenKind::Minus: return "-";
        case TokenKind::Star: return "*";
        case TokenKind::Slash: return "/";
        case TokenKind::Percent: return "%";
        case TokenKind::Less: return "<";
        case TokenKind::Greater: return ">";
        case TokenKind::LessEqual: return "<=";
        case TokenKind::GreaterEqual: return ">=";
        case TokenKind::EqualEqual: return "==";
        case TokenKind::EqualEqualEqual: return "===";
        case TokenKind::BangEqual: return "!=";
        case TokenKind::BangEqualEqual: return "!==";
        case TokenKind::AmpAmp: return "&&";
        case TokenKind::PipePipe: return "||";
        case TokenKind::Question: return "?";
        case TokenKind::QuestionQuestion: return "??";
        case TokenKind::Bang: return "!";
        case TokenKind::PlusPlus: return "++";
        case TokenKind::MinusMinus: return "--";
        case TokenKind::PlusAssign: return "+=";
        case TokenKind::MinusAssign: return "-=";
        case TokenKind::StarAssign: return "*=";
        case TokenKind::SlashAssign: return "/=";
        case TokenKind::PercentAssign: return "%=";
    }
    return "unknown";
}

static bool isIdentStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}
static bool isIdentPart(char c) {
    return isIdentStart(c) || (c >= '0' && c <= '9');
}
static bool isDigit(char c) { return c >= '0' && c <= '9'; }

char Lexer::peek(uint32_t ahead) const {
    const auto text = buffer_.text();
    const uint64_t idx = static_cast<uint64_t>(pos_) + ahead;
    return idx < text.size() ? text[static_cast<size_t>(idx)] : '\0';
}

bool Lexer::atEnd() const { return pos_ >= buffer_.text().size(); }

Token Lexer::make(TokenKind kind, uint32_t begin) const {
    return Token{kind, {begin, pos_}, buffer_.text().substr(begin, pos_ - begin)};
}

void Lexer::skipTrivia() {
    for (;;) {
        const char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++pos_;
        } else if (c == '/' && peek(1) == '/') {
            while (!atEnd() && peek() != '\n') ++pos_;
        } else if (c == '/' && peek(1) == '*') {
            const uint32_t begin = pos_;
            pos_ += 2;
            while (!atEnd() && !(peek() == '*' && peek(1) == '/')) ++pos_;
            if (atEnd()) {
                diags_.error({begin, pos_}, "unterminated block comment");
                return;
            }
            pos_ += 2;
        } else {
            return;
        }
    }
}

Token Lexer::lexIdentifierOrKeyword() {
    const uint32_t begin = pos_;
    while (isIdentPart(peek())) ++pos_;
    const auto text = buffer_.text().substr(begin, pos_ - begin);
    struct Keyword {
        std::string_view text;
        TokenKind kind;
    };
    static constexpr Keyword kKeywords[] = {
        {"break", TokenKind::KwBreak},       {"case", TokenKind::KwCase},
        {"catch", TokenKind::KwCatch},       {"const", TokenKind::KwConst},
        {"continue", TokenKind::KwContinue}, {"default", TokenKind::KwDefault},
        {"delete", TokenKind::KwDelete},
        {"do", TokenKind::KwDo},             {"else", TokenKind::KwElse},
        {"export", TokenKind::KwExport},     {"false", TokenKind::KwFalse},
        {"for", TokenKind::KwFor},           {"function", TokenKind::KwFunction},
        {"if", TokenKind::KwIf},             {"import", TokenKind::KwImport},
        {"in", TokenKind::KwIn},             {"let", TokenKind::KwLet},
        {"new", TokenKind::KwNew},           {"null", TokenKind::KwNull},
        {"of", TokenKind::KwOf},             {"return", TokenKind::KwReturn},
        {"switch", TokenKind::KwSwitch},     {"throw", TokenKind::KwThrow},
        {"true", TokenKind::KwTrue},         {"try", TokenKind::KwTry},
        {"this", TokenKind::KwThis},         {"undefined", TokenKind::KwUndefined},
        {"var", TokenKind::KwVar},
        {"while", TokenKind::KwWhile},
    };
    for (const auto& kw : kKeywords) {
        if (text == kw.text) return make(kw.kind, begin);
    }
    return make(TokenKind::Identifier, begin);
}

Token Lexer::lexNumber() {
    const uint32_t begin = pos_;
    while (isDigit(peek())) ++pos_;
    if (peek() == '.' && isDigit(peek(1))) {
        ++pos_;
        while (isDigit(peek())) ++pos_;
    }
    return make(TokenKind::NumberLiteral, begin);
}

Token Lexer::lexString() {
    const uint32_t begin = pos_;
    const char quote = peek();
    ++pos_;
    while (!atEnd() && peek() != quote && peek() != '\n') {
        if (peek() == '\\') ++pos_;  // escape: consume the backslash + next
        ++pos_;
    }
    if (atEnd() || peek() != quote) {
        diags_.error({begin, pos_}, "unterminated string literal");
        return make(TokenKind::StringLiteral, begin);
    }
    ++pos_;
    return make(TokenKind::StringLiteral, begin);
}

Token Lexer::lexPunctuation() {
    const uint32_t begin = pos_;
    const char c = peek();
    switch (c) {
        case '(': ++pos_; return make(TokenKind::LParen, begin);
        case ')': ++pos_; return make(TokenKind::RParen, begin);
        case '{': ++pos_; return make(TokenKind::LBrace, begin);
        case '}': ++pos_; return make(TokenKind::RBrace, begin);
        case '[': ++pos_; return make(TokenKind::LBracket, begin);
        case ']': ++pos_; return make(TokenKind::RBracket, begin);
        case ',': ++pos_; return make(TokenKind::Comma, begin);
        case ';': ++pos_; return make(TokenKind::Semicolon, begin);
        case ':': ++pos_; return make(TokenKind::Colon, begin);
        case '.': ++pos_; return make(TokenKind::Dot, begin);
        case '?':
            if (peek(1) == '?') { pos_ += 2; return make(TokenKind::QuestionQuestion, begin); }
            ++pos_; return make(TokenKind::Question, begin);
        case '&':
            if (peek(1) == '&') { pos_ += 2; return make(TokenKind::AmpAmp, begin); }
            break;
        case '|':
            if (peek(1) == '|') { pos_ += 2; return make(TokenKind::PipePipe, begin); }
            break;
        case '+':
            if (peek(1) == '+') { pos_ += 2; return make(TokenKind::PlusPlus, begin); }
            if (peek(1) == '=') { pos_ += 2; return make(TokenKind::PlusAssign, begin); }
            ++pos_; return make(TokenKind::Plus, begin);
        case '-':
            if (peek(1) == '-') { pos_ += 2; return make(TokenKind::MinusMinus, begin); }
            if (peek(1) == '=') { pos_ += 2; return make(TokenKind::MinusAssign, begin); }
            ++pos_; return make(TokenKind::Minus, begin);
        case '*':
            if (peek(1) == '=') { pos_ += 2; return make(TokenKind::StarAssign, begin); }
            ++pos_; return make(TokenKind::Star, begin);
        case '/':
            if (peek(1) == '=') { pos_ += 2; return make(TokenKind::SlashAssign, begin); }
            ++pos_; return make(TokenKind::Slash, begin);
        case '%':
            if (peek(1) == '=') { pos_ += 2; return make(TokenKind::PercentAssign, begin); }
            ++pos_; return make(TokenKind::Percent, begin);
        case '<':
            if (peek(1) == '=') { pos_ += 2; return make(TokenKind::LessEqual, begin); }
            ++pos_; return make(TokenKind::Less, begin);
        case '>':
            if (peek(1) == '=') { pos_ += 2; return make(TokenKind::GreaterEqual, begin); }
            ++pos_; return make(TokenKind::Greater, begin);
        case '=':
            if (peek(1) == '>') { pos_ += 2; return make(TokenKind::Arrow, begin); }
            if (peek(1) == '=' && peek(2) == '=') { pos_ += 3; return make(TokenKind::EqualEqualEqual, begin); }
            if (peek(1) == '=') { pos_ += 2; return make(TokenKind::EqualEqual, begin); }
            ++pos_;
            return make(TokenKind::Assign, begin);
        case '!':
            if (peek(1) == '=' && peek(2) == '=') { pos_ += 3; return make(TokenKind::BangEqualEqual, begin); }
            if (peek(1) == '=') { pos_ += 2; return make(TokenKind::BangEqual, begin); }
            ++pos_;
            return make(TokenKind::Bang, begin);
        default: break;
    }
    ++pos_;
    diags_.error({begin, pos_}, std::string("unrecognized character '") + c + "'");
    return make(TokenKind::EndOfFile, begin);
}

std::vector<Token> Lexer::lex() {
    std::vector<Token> tokens;
    for (;;) {
        skipTrivia();
        if (atEnd() || diags_.hasErrors()) break;
        const char c = peek();
        if (isIdentStart(c)) {
            tokens.push_back(lexIdentifierOrKeyword());
        } else if (isDigit(c)) {
            tokens.push_back(lexNumber());
        } else if (c == '"' || c == '\'') {
            tokens.push_back(lexString());
        } else {
            tokens.push_back(lexPunctuation());
        }
    }
    tokens.push_back(Token{TokenKind::EndOfFile, {pos_, pos_}, {}});
    return tokens;
}

}  // namespace bronze
