#include "lex/lexer.h"

namespace bronze {

const char* tokenKindName(TokenKind kind) {
    switch (kind) {
        case TokenKind::EndOfFile: return "eof";
        case TokenKind::Identifier: return "ident";
        case TokenKind::NumberLiteral: return "number";
        case TokenKind::StringLiteral: return "string";
        case TokenKind::TemplateWhole: return "template";
        case TokenKind::TemplateHead: return "template-head";
        case TokenKind::TemplateMiddle: return "template-middle";
        case TokenKind::TemplateTail: return "template-tail";
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
        case TokenKind::KwFinally: return "finally";
        case TokenKind::KwFor: return "for";
        case TokenKind::KwFunction: return "function";
        case TokenKind::KwIf: return "if";
        case TokenKind::KwImport: return "import";
        case TokenKind::KwIn: return "in";
        case TokenKind::KwInstanceof: return "instanceof";
        case TokenKind::KwTypeof: return "typeof";
        case TokenKind::KwVoid: return "void";
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
        case TokenKind::Ellipsis: return "...";
        case TokenKind::KwClass: return "class";
        case TokenKind::KwExtends: return "extends";
        case TokenKind::KwSuper: return "super";
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
        case TokenKind::QuestionDot: return "?.";
        case TokenKind::Bang: return "!";
        case TokenKind::PlusPlus: return "++";
        case TokenKind::MinusMinus: return "--";
        case TokenKind::PlusAssign: return "+=";
        case TokenKind::MinusAssign: return "-=";
        case TokenKind::StarAssign: return "*=";
        case TokenKind::SlashAssign: return "/=";
        case TokenKind::PercentAssign: return "%=";
        case TokenKind::Amp: return "&";
        case TokenKind::Pipe: return "|";
        case TokenKind::Caret: return "^";
        case TokenKind::Tilde: return "~";
        case TokenKind::LessLess: return "<<";
        case TokenKind::GreaterGreater: return ">>";
        case TokenKind::GreaterGreaterGreater: return ">>>";
        case TokenKind::AmpAssign: return "&=";
        case TokenKind::PipeAssign: return "|=";
        case TokenKind::CaretAssign: return "^=";
        case TokenKind::LessLessAssign: return "<<=";
        case TokenKind::GreaterGreaterAssign: return ">>=";
        case TokenKind::GreaterGreaterGreaterAssign: return ">>>=";
        case TokenKind::StarStar: return "**";
        case TokenKind::StarStarAssign: return "**=";
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
static bool isHexDigit(char c) {
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

char Lexer::peek(uint32_t ahead) const {
    const auto text = buffer_.text();
    const uint64_t idx = static_cast<uint64_t>(pos_) + ahead;
    return idx < text.size() ? text[static_cast<size_t>(idx)] : '\0';
}

bool Lexer::atEnd() const { return pos_ >= buffer_.text().size(); }

// Every token span carries the id of the buffer it indexes. This is the one
// place a span is built from raw offsets, so stamping it here is what makes a
// diagnostic about a file other than the entry render against that file's
// text (docs/0023 decision 1).
Token Lexer::make(TokenKind kind, uint32_t begin) const {
    return Token{kind,
                 {begin, pos_, buffer_.fileId()},
                 buffer_.text().substr(begin, pos_ - begin)};
}

bool Lexer::skipTrivia() {
    bool sawNewline = false;
    for (;;) {
        const char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (c == '\n') sawNewline = true;
            ++pos_;
        } else if (c == '/' && peek(1) == '/') {
            // The terminating newline is left for the branch above, so a line
            // comment reports through it like any other newline.
            while (!atEnd() && peek() != '\n') ++pos_;
        } else if (c == '/' && peek(1) == '*') {
            const uint32_t begin = pos_;
            pos_ += 2;
            while (!atEnd() && !(peek() == '*' && peek(1) == '/')) {
                // A block comment spanning lines IS a line terminator for the
                // purposes of ASI (ECMA-262 12.4): `return /*\n*/ 1` returns
                // undefined, exactly as `return\n1` does.
                if (peek() == '\n') sawNewline = true;
                ++pos_;
            }
            if (atEnd()) {
                diags_.error({begin, pos_, buffer_.fileId()}, "unterminated block comment");
                return sawNewline;
            }
            pos_ += 2;
        } else {
            return sawNewline;
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
        {"class", TokenKind::KwClass},     {"extends", TokenKind::KwExtends},
        {"super", TokenKind::KwSuper},
        {"do", TokenKind::KwDo},             {"else", TokenKind::KwElse},
        {"export", TokenKind::KwExport},     {"false", TokenKind::KwFalse},
        {"for", TokenKind::KwFor},           {"function", TokenKind::KwFunction},
        {"finally", TokenKind::KwFinally},
        {"if", TokenKind::KwIf},             {"import", TokenKind::KwImport},
        {"in", TokenKind::KwIn},             {"instanceof", TokenKind::KwInstanceof},
        {"typeof", TokenKind::KwTypeof},     {"void", TokenKind::KwVoid},
        {"let", TokenKind::KwLet},
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

// Where a numeric literal ENDS. What it denotes — the radix, the digits, the
// separators — is `decodeNumericLiteral`'s to say, the same division of labour
// string literals already have: a token whose text disagreed with its span
// about what it describes would make every diagnostic about it point at the
// wrong characters.
//
// So this is deliberately permissive. `0b19`, `1__0` and `1_` are each ONE
// token here and each a named error there, which is what lets the message
// quote the whole literal. Being permissive is also what stops `0xFF` from
// lexing as `0` followed by an identifier `xFF`, which is what it did.
Token Lexer::lexNumber() {
    const uint32_t begin = pos_;
    // A separator is legal only between digits, but it has to be CONSUMED
    // wherever it appears, or `1_` would lex as `1` and an identifier `_`.
    const auto takeDigits = [&](bool (*isPart)(char)) {
        while (isPart(peek()) || peek() == '_') ++pos_;
    };

    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X' || peek(1) == 'o' ||
                          peek(1) == 'O' || peek(1) == 'b' || peek(1) == 'B')) {
        pos_ += 2;
        // Hex digits are a superset of octal's and binary's, so one scan finds
        // the end of all three and the parser rejects a digit the radix does
        // not have.
        takeDigits(isHexDigit);
        return make(TokenKind::NumberLiteral, begin);
    }

    takeDigits(isDigit);
    // `1.foo` is a property access on a number, not a literal ending in a
    // dot, so the dot is only part of the literal when a digit (or a
    // separator, which is an error the parser names) follows it.
    if (peek() == '.' && (isDigit(peek(1)) || peek(1) == '_')) {
        ++pos_;
        takeDigits(isDigit);
    }
    // An ExponentPart, which needs at least one digit after the optional
    // sign — otherwise the `e` is the start of an identifier and `1e` is two
    // tokens, as it always was. A separator counts as the start of one so
    // that `1e_5` is a single literal the parser can name the real fault in,
    // rather than a number followed by an identifier `_5`.
    const auto startsExponent = [&](char c) { return isDigit(c) || c == '_'; };
    if ((peek() == 'e' || peek() == 'E') &&
        (startsExponent(peek(1)) ||
         ((peek(1) == '+' || peek(1) == '-') && startsExponent(peek(2))))) {
        pos_ += 2;  // the `e`, and the sign or first digit
        takeDigits(isDigit);
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
        diags_.error({begin, pos_, buffer_.fileId()}, "unterminated string literal");
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
        case '.':
            // `...` is one token: rest and spread are the only things it
            // spells, and the parser can only name them if it sees them as
            // one (three Dots read as a property access of nothing).
            if (peek(1) == '.' && peek(2) == '.') { pos_ += 3; return make(TokenKind::Ellipsis, begin); }
            ++pos_; return make(TokenKind::Dot, begin);
        case '?':
            if (peek(1) == '?') { pos_ += 2; return make(TokenKind::QuestionQuestion, begin); }
            // ECMA-262 12.8 spells the optional-chaining punctuator
            // `?.` [lookahead ∉ DecimalDigit], and the lookahead is the whole
            // point: `x ? .5 : .25` is a ternary over two fractions, and a
            // greedy `?.` would read its first arm as an optional member
            // access of nothing.
            if (peek(1) == '.' && !(peek(2) >= '0' && peek(2) <= '9')) {
                pos_ += 2;
                return make(TokenKind::QuestionDot, begin);
            }
            ++pos_; return make(TokenKind::Question, begin);
        case '&':
            if (peek(1) == '&') { pos_ += 2; return make(TokenKind::AmpAmp, begin); }
            if (peek(1) == '=') { pos_ += 2; return make(TokenKind::AmpAssign, begin); }
            ++pos_; return make(TokenKind::Amp, begin);
        case '|':
            if (peek(1) == '|') { pos_ += 2; return make(TokenKind::PipePipe, begin); }
            if (peek(1) == '=') { pos_ += 2; return make(TokenKind::PipeAssign, begin); }
            ++pos_; return make(TokenKind::Pipe, begin);
        case '^':
            if (peek(1) == '=') { pos_ += 2; return make(TokenKind::CaretAssign, begin); }
            ++pos_; return make(TokenKind::Caret, begin);
        case '~':
            ++pos_; return make(TokenKind::Tilde, begin);
        case '+':
            if (peek(1) == '+') { pos_ += 2; return make(TokenKind::PlusPlus, begin); }
            if (peek(1) == '=') { pos_ += 2; return make(TokenKind::PlusAssign, begin); }
            ++pos_; return make(TokenKind::Plus, begin);
        case '-':
            if (peek(1) == '-') { pos_ += 2; return make(TokenKind::MinusMinus, begin); }
            if (peek(1) == '=') { pos_ += 2; return make(TokenKind::MinusAssign, begin); }
            ++pos_; return make(TokenKind::Minus, begin);
        case '*':
            // The longest match wins, always: `**=` before `**` before `*=`.
            // Reading `**` as two `*` would make `2 ** 3` a multiplication by
            // nothing rather than an exponentiation.
            if (peek(1) == '*' && peek(2) == '=') { pos_ += 3; return make(TokenKind::StarStarAssign, begin); }
            if (peek(1) == '*') { pos_ += 2; return make(TokenKind::StarStar, begin); }
            if (peek(1) == '=') { pos_ += 2; return make(TokenKind::StarAssign, begin); }
            ++pos_; return make(TokenKind::Star, begin);
        case '/':
            if (peek(1) == '=') { pos_ += 2; return make(TokenKind::SlashAssign, begin); }
            ++pos_; return make(TokenKind::Slash, begin);
        case '%':
            if (peek(1) == '=') { pos_ += 2; return make(TokenKind::PercentAssign, begin); }
            ++pos_; return make(TokenKind::Percent, begin);
        case '<':
            if (peek(1) == '<' && peek(2) == '=') { pos_ += 3; return make(TokenKind::LessLessAssign, begin); }
            if (peek(1) == '<') { pos_ += 2; return make(TokenKind::LessLess, begin); }
            if (peek(1) == '=') { pos_ += 2; return make(TokenKind::LessEqual, begin); }
            ++pos_; return make(TokenKind::Less, begin);
        case '>':
            // Four spellings start with `>>`, so they are tried longest first.
            // bronze has no generics in its annotation grammar (one identifier,
            // parser_stmt.cpp), so nothing else wants `>>` split in two.
            if (peek(1) == '>' && peek(2) == '>' && peek(3) == '=') {
                pos_ += 4; return make(TokenKind::GreaterGreaterGreaterAssign, begin);
            }
            if (peek(1) == '>' && peek(2) == '>') { pos_ += 3; return make(TokenKind::GreaterGreaterGreater, begin); }
            if (peek(1) == '>' && peek(2) == '=') { pos_ += 3; return make(TokenKind::GreaterGreaterAssign, begin); }
            if (peek(1) == '>') { pos_ += 2; return make(TokenKind::GreaterGreater, begin); }
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
    diags_.error({begin, pos_, buffer_.fileId()}, std::string("unrecognized character '") + c + "'");
    return make(TokenKind::EndOfFile, begin);
}

// One piece of a template literal, from the opening delimiter (a backtick
// for the head, the `}` that closed the previous substitution otherwise) to
// whichever delimiter ends it: `${` starts a substitution, a backtick ends
// the template.
//
// The substitution's own tokens are lexed by the main loop, which is what
// `substitutionBraces_` tracks: a `}` closes the substitution only when no
// object literal or block inside it is still open.
Token Lexer::lexTemplatePart(bool isHead) {
    const uint32_t begin = pos_;
    ++pos_;  // the ` or }
    for (;;) {
        if (atEnd()) {
            diags_.error({begin, pos_, buffer_.fileId()}, "unterminated template literal");
            return make(TokenKind::TemplateWhole, begin);
        }
        const char c = peek();
        if (c == '\\') {
            pos_ += 2;  // escape: consume the backslash and whatever follows
            continue;
        }
        if (c == '`') {
            ++pos_;
            return make(isHead ? TokenKind::TemplateWhole : TokenKind::TemplateTail, begin);
        }
        if (c == '$' && peek(1) == '{') {
            pos_ += 2;
            substitutionBraces_.push_back(0);
            return make(isHead ? TokenKind::TemplateHead : TokenKind::TemplateMiddle, begin);
        }
        ++pos_;  // a newline inside a template is content, not a terminator
    }
}

std::vector<Token> Lexer::lex() {
    std::vector<Token> tokens;
    bool newlineBefore = false;
    for (;;) {
        newlineBefore = skipTrivia();
        if (atEnd() || diags_.hasErrors()) break;
        const char c = peek();
        if (isIdentStart(c)) {
            tokens.push_back(lexIdentifierOrKeyword());
        } else if (isDigit(c) || (c == '.' && isDigit(peek(1)))) {
            // A DecimalLiteral may begin with the point (`.5`). The digit
            // lookahead is what keeps `...` a spread and `a.b` a member
            // access.
            tokens.push_back(lexNumber());
        } else if (c == '"' || c == '\'') {
            tokens.push_back(lexString());
        } else if (c == '`') {
            tokens.push_back(lexTemplatePart(/*isHead=*/true));
        } else if (c == '}' && !substitutionBraces_.empty() && substitutionBraces_.back() == 0) {
            // This `}` closes the innermost template substitution rather
            // than a block or an object literal, so the template it
            // interrupted resumes here.
            substitutionBraces_.pop_back();
            tokens.push_back(lexTemplatePart(/*isHead=*/false));
        } else {
            if (!substitutionBraces_.empty()) {
                if (c == '{') ++substitutionBraces_.back();
                else if (c == '}') --substitutionBraces_.back();
            }
            tokens.push_back(lexPunctuation());
        }
        tokens.back().newlineBefore = newlineBefore;
    }
    tokens.push_back(
        Token{TokenKind::EndOfFile, {pos_, pos_, buffer_.fileId()}, {}, newlineBefore});
    return tokens;
}

}  // namespace bronze
