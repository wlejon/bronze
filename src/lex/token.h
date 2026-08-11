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
    // Template literals. A template is lexed as a SEQUENCE — the pieces
    // between the substitutions, with the substitutions' own tokens in
    // between — because a substitution holds an arbitrary expression, which
    // may itself contain a template. One token per whole template would
    // mean re-lexing its interior from a detached string, and every span
    // inside it would point at the wrong place.
    //   `abc`            -> TemplateWhole
    //   `a${x}b${y}c`    -> TemplateHead x TemplateMiddle y TemplateTail
    TemplateWhole,
    TemplateHead,
    TemplateMiddle,
    TemplateTail,

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
    KwInstanceof,
    KwLet,
    KwNew,
    KwNull,
    KwOf,
    KwReturn,
    KwSwitch,
    KwThrow,
    KwTrue,
    KwTry,
    KwTypeof,
    KwVoid,
    KwDelete,
    KwClass,
    KwExtends,
    KwSuper,
    KwThis,
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
    Ellipsis,  // ...
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
    // Bitwise and shifts. `&` and `|` were previously lexed only as the
    // second character of `&&` and `||`, so `a & b` reported an unrecognized
    // character and named nothing (docs/0015).
    Amp,          // &
    Pipe,         // |
    Caret,        // ^
    Tilde,        // ~
    LessLess,     // <<
    GreaterGreater,        // >>
    GreaterGreaterGreater, // >>>
    AmpAssign,             // &=
    PipeAssign,            // |=
    CaretAssign,           // ^=
    LessLessAssign,        // <<=
    GreaterGreaterAssign,  // >>=
    GreaterGreaterGreaterAssign,  // >>>=
    StarStar,       // **
    StarStarAssign, // **=
};

const char* tokenKindName(TokenKind kind);

struct Token {
    TokenKind kind;
    Span span;
    std::string_view text;  // view into the SourceBuffer
    // Whether a line terminator separates this token from the one before it.
    // Trivia is discarded here, so the lexer is the only place that can still
    // see it, and automatic semicolon insertion is the only thing that needs
    // it — the grammar is otherwise whitespace-insensitive (docs/0014).
    bool newlineBefore = false;
};

}  // namespace bronze
