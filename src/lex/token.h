#pragma once
#include <string_view>

#include "support/source.h"

namespace bronze {

// Token kinds for the TypeScript core. Grows as the parser grows; the lexer
// hard-errors (via DiagnosticSink) on anything it does not recognize.
enum class TokenKind {
    EndOfFile,
    Identifier,
    NumberLiteral,
    StringLiteral,

    // Keywords (kept alphabetical).
    KwBreak,
    KwCase,
    KwCatch,
    KwConst,
    KwContinue,
    KwDefault,
    KwDo,
    KwElse,
    KwExport,
    KwFalse,
    KwFor,
    KwFunction,
    KwIf,
    KwImport,
    KwIn,
    KwLet,
    KwNull,
    KwOf,
    KwReturn,
    KwSwitch,
    KwThrow,
    KwTrue,
    KwTry,
    KwUndefined,
    KwVar,
    KwWhile,

    // Punctuation / operators.
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Comma,
    Semicolon,
    Colon,
    Dot,
    Arrow,        // =>
    Assign,       // =
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Less,
    Greater,
    LessEqual,
    GreaterEqual,
    EqualEqual,   // ==
    EqualEqualEqual,  // ===
    BangEqual,    // !=
    BangEqualEqual,   // !==
    AmpAmp,       // &&
    PipePipe,     // ||
    Question,     // ?
    QuestionQuestion, // ??
    Bang,         // !
    PlusPlus,     // ++
    MinusMinus,   // --
    PlusAssign,   // +=
    MinusAssign,  // -=
    StarAssign,   // *=
    SlashAssign,  // /=
    PercentAssign,// %=
};

const char* tokenKindName(TokenKind kind);

struct Token {
    TokenKind kind;
    Span span;
    std::string_view text;  // view into the SourceBuffer
};

}  // namespace bronze
