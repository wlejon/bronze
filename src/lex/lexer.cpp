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
        case TokenKind::RegExpLiteral: return "regexp";
        case TokenKind::PrivateName: return "private-name";
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
        case TokenKind::PipePipeAssign: return "||=";
        case TokenKind::AmpAmpAssign: return "&&=";
        case TokenKind::QuestionQuestionAssign: return "??=";
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
// diagnostic about a file other than the entry render against that file's text.
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
            // <CR> is a LineTerminator (ECMA-262 12.3), so a CRLF or lone-CR
            // file triggers ASI exactly as an LF file does.
            if (c == '\n' || c == '\r') sawNewline = true;
            ++pos_;
        } else if (static_cast<unsigned char>(c) == 0xEF &&
                   static_cast<unsigned char>(peek(1)) == 0xBB &&
                   static_cast<unsigned char>(peek(2)) == 0xBF) {
            // U+FEFF encoded as UTF-8. ECMA-262 12.2 lists <ZWNBSP> in
            // WhiteSpace, so it is trivia wherever it appears and not only as
            // a leading signature — which is also why this is one rule here
            // rather than a special case at offset zero. It is NOT a line
            // terminator (12.3 lists those separately), so it leaves ASI
            // alone. Editors emit it at the head of a file; the source is
            // otherwise rejected at its first character.
            pos_ += 3;
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
        if (peek() == 'n' || peek() == 'N') ++pos_;
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
    // The BigInt suffix, consumed even after a fraction or an exponent — where
    // the grammar does not allow it — for the reason the whole of this function
    // is permissive: `1.5n` and `1e3n` are errors that must be NAMED, and a
    // diagnostic can only quote a literal it was handed whole. Leaving the `n`
    // behind made them lex as a number followed by the identifier `n`, and the
    // message that came out was about a stray identifier.
    if (peek() == 'n' || peek() == 'N') ++pos_;
    return make(TokenKind::NumberLiteral, begin);
}

Token Lexer::lexString() {
    const uint32_t begin = pos_;
    const char quote = peek();
    ++pos_;
    // <CR> terminates like <LF>: both are LineTerminators, and a quoted
    // literal cannot contain an unescaped one (ECMA-262 12.9.4).
    while (!atEnd() && peek() != quote && peek() != '\n' && peek() != '\r') {
        if (peek() == '\\') {
            ++pos_;  // escape: consume the backslash, then what it escapes
            // An escaped CRLF is one LineContinuation: take both characters,
            // or the loop would read the LF half as a terminator.
            if (peek() == '\r' && peek(1) == '\n') ++pos_;
        }
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
            if (peek(1) == '?' && peek(2) == '=') { pos_ += 3; return make(TokenKind::QuestionQuestionAssign, begin); }
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
            if (peek(1) == '&' && peek(2) == '=') { pos_ += 3; return make(TokenKind::AmpAmpAssign, begin); }
            if (peek(1) == '&') { pos_ += 2; return make(TokenKind::AmpAmp, begin); }
            if (peek(1) == '=') { pos_ += 2; return make(TokenKind::AmpAssign, begin); }
            ++pos_; return make(TokenKind::Amp, begin);
        case '|':
            if (peek(1) == '|' && peek(2) == '=') { pos_ += 3; return make(TokenKind::PipePipeAssign, begin); }
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

// The `/` ambiguity, decided the only way a lexer can decide it: `a / b`
// divides and `/ab/` is a literal, and the two are told apart by whether the
// token before the slash could END an expression. Everything that could is
// listed here; everything else — an operator, an opening bracket, a keyword
// that must be followed by an expression, the start of the file — leaves a
// regular expression the only reading.
//
// Two entries are judgement calls the specification leaves to the parser,
// which knows more than this does:
//
//  - `)` divides. `(a + b) / 2` is overwhelmingly what a `)` before a slash
//    means; `if (x) /re/.test(y)` is the case this gets wrong, and it needs
//    the parenthesis's OWN opener to be known, which is a parser fact.
//  - `}` starts a regular expression. A `}` almost always ends a block or a
//    function body, after which a slash begins a statement; `({}) / 2` is
//    the reading this gives up, and it needs a `{` disambiguated as an object
//    literal, which is again a parser fact.
//
// Both are pinned in tests/lex so a change of mind is a change of test.
bool Lexer::regexAllowedAfter(const std::vector<Token>& tokens) {
    if (tokens.empty()) return true;
    switch (tokens.back().kind) {
        case TokenKind::Identifier:
        case TokenKind::NumberLiteral:
        case TokenKind::StringLiteral:
        case TokenKind::TemplateWhole:
        case TokenKind::TemplateTail:
        case TokenKind::RegExpLiteral:
        case TokenKind::RParen:
        case TokenKind::RBracket:
        // `a++ / b` divides: the operand is behind the operator, so the
        // increment is what ends the expression.
        case TokenKind::PlusPlus:
        case TokenKind::MinusMinus:
        case TokenKind::KwThis:
        case TokenKind::KwSuper:
        case TokenKind::KwTrue:
        case TokenKind::KwFalse:
        case TokenKind::KwNull:
        case TokenKind::KwUndefined:
            return false;
        default:
            return true;
    }
}

// From the opening `/` to the flags. The body is scanned, never interpreted:
// a `\` escapes whatever follows it (including a `/`), and a `/` inside a
// character class is an ordinary character, which is why the class bracket has
// to be tracked here even though nothing else about the class matters.
Token Lexer::lexRegExp() {
    const uint32_t begin = pos_;
    ++pos_;  // the opening '/'
    bool inClass = false;
    for (;;) {
        if (atEnd() || peek() == '\n' || peek() == '\r') {
            diags_.error({begin, pos_, buffer_.fileId()},
                         "unterminated regular expression literal");
            return make(TokenKind::RegExpLiteral, begin);
        }
        const char c = peek();
        if (c == '\\') {
            pos_ += 2;
            continue;
        }
        if (c == '[') inClass = true;
        else if (c == ']') inClass = false;
        else if (c == '/' && !inClass) break;
        ++pos_;
    }
    ++pos_;  // the closing '/'
    // The flags are an IdentifierPart run, which is what makes `/a/gi` one
    // token and `/a/ x` two. A letter that is not a flag is the pattern
    // compiler's error to name, not this one's.
    while (isIdentPart(peek())) ++pos_;
    return make(TokenKind::RegExpLiteral, begin);
}

std::vector<Token> Lexer::lex() {
    std::vector<Token> tokens;
    bool newlineBefore = false;
    // 12.5 HashbangComment. It is a grammar production of Script and Module
    // rather than a comment the trivia scanner recognises, and the difference is
    // exactly this: it is legal at offset ZERO and nowhere else, so a `#!` after
    // even one space or one blank line is still the stray-character error below.
    // Checked before the first `skipTrivia` for that reason.
    if (pos_ == 0 && peek() == '#' && peek(1) == '!') {
        // To the end of the line, not past it: the line terminator is trivia
        // the loop's own skip consumes, and consuming it here would lose the
        // `newlineBefore` the first real token needs.
        while (!atEnd() && peek() != '\n' && peek() != '\r') ++pos_;
    }
    for (;;) {
        newlineBefore = skipTrivia();
        if (atEnd() || diags_.hasErrors()) break;
        const char c = peek();
        if (isIdentStart(c)) {
            tokens.push_back(lexIdentifierOrKeyword());
        } else if (c == '#' && isIdentStart(peek(1))) {
            // 12.7.2 PrivateIdentifier: `#` and the name it prefixes are one
            // token, and the `#` alone is not a token at all — a `#` followed
            // by anything else still reaches lexPunctuation's refusal below,
            // so `a # b` names the stray character rather than an empty
            // private name.
            const uint32_t begin = pos_;
            ++pos_;  // '#'
            while (isIdentPart(peek())) ++pos_;
            tokens.push_back(make(TokenKind::PrivateName, begin));
        } else if (isDigit(c) || (c == '.' && isDigit(peek(1)))) {
            // A DecimalLiteral may begin with the point (`.5`). The digit
            // lookahead is what keeps `...` a spread and `a.b` a member
            // access.
            tokens.push_back(lexNumber());
        } else if (c == '"' || c == '\'') {
            tokens.push_back(lexString());
        } else if (c == '`') {
            tokens.push_back(lexTemplatePart(/*isHead=*/true));
        } else if (c == '/' && regexAllowedAfter(tokens)) {
            // Comments are already gone: skipTrivia consumed `//` and `/*`
            // before this loop saw anything, so a `/` here is either a
            // division or the start of a literal.
            tokens.push_back(lexRegExp());
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
