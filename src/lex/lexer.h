#pragma once
#include <vector>

#include "lex/token.h"
#include "support/diagnostics.h"
#include "support/source.h"

namespace bronze {

// Hand-written single-pass lexer over a SourceBuffer. Produces the full
// token vector (always terminated by EndOfFile). Unrecognized input is a
// diagnosed error, never skipped silently.
class Lexer {
public:
    Lexer(const SourceBuffer& buffer, DiagnosticSink& diags)
        : buffer_(buffer), diags_(diags) {}

    std::vector<Token> lex();

private:
    const SourceBuffer& buffer_;
    DiagnosticSink& diags_;
    uint32_t pos_ = 0;
    // One entry per template substitution currently open, counting the
    // braces opened inside it. Empty means `}` is just a `}`.
    std::vector<uint32_t> substitutionBraces_;

    char peek(uint32_t ahead = 0) const;
    bool atEnd() const;
    Token make(TokenKind kind, uint32_t begin) const;
    // True when the trivia it consumed contained a line terminator.
    bool skipTrivia();
    Token lexIdentifierOrKeyword();
    Token lexNumber();
    Token lexString();
    Token lexTemplatePart(bool isHead);
    Token lexRegExp();
    Token lexPunctuation();

    // Whether a `/` here begins a regular expression literal rather than a
    // division. Nothing but the previous significant token decides it, which
    // is the classic JavaScript ambiguity and the one place this lexer is not
    // context-free (docs/0024 decision 1).
    static bool regexAllowedAfter(const std::vector<Token>& tokens);
};

}  // namespace bronze
