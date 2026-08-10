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

    // Keywords (kept alphabetical; keywordFromText must match).
    KwConst,
    KwElse,
    KwExport,
    KwFunction,
    KwIf,
    KwImport,
    KwLet,
    KwReturn,

    // Punctuation / operators.
    LParen,
    RParen,
    LBrace,
    RBrace,
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
    Less,
    Greater,
    EqualEqual,   // ==
    EqualEqualEqual,  // ===
    BangEqual,    // !=
    BangEqualEqual,   // !==
};

const char* tokenKindName(TokenKind kind);

struct Token {
    TokenKind kind;
    Span span;
    std::string_view text;  // view into the SourceBuffer
};

}  // namespace bronze
