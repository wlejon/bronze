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

    char peek(uint32_t ahead = 0) const;
    bool atEnd() const;
    Token make(TokenKind kind, uint32_t begin) const;
    void skipTrivia();
    Token lexIdentifierOrKeyword();
    Token lexNumber();
    Token lexString();
    Token lexPunctuation();
};

}  // namespace bronze
