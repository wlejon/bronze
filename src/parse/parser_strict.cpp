// Strict mode: the Directive Prologue that selects it (ECMA-262 11.2.2) and
// the early errors that only strict code has (8.6.2 / 13.5.1.2 `delete` of an
// identifier, 12.7.2 the future reserved words, 13.15.1 `eval` and `arguments`
// as targets, 15.1.2 / 15.2.1 duplicate parameters).
//
// Its own unit because it is one grammar-level concept — "which mode is this
// code in, and what is therefore not writable here" — reached from every other
// parser unit. The alternative was the same eight checks copied into
// parser_stmt, parser_expr, parser_func and parser_pattern, which is four
// places for the rules to drift apart.

#include <string>

#include "parse/parser.h"

namespace bronze {

namespace {

// The nine words 12.7.2 reserves in strict code and nowhere else. `let` is
// listed even though bronze's lexer already makes it a keyword token: this
// table is the specification's list, and a reader checking it against the
// standard must find all nine.
constexpr std::string_view kStrictReservedWords[] = {
    "implements", "interface", "let",       "package", "private",
    "protected",  "public",    "static",    "yield",
};

// Can this token continue an ExpressionStatement that a string literal has
// just started? It decides whether the literal is a whole statement — and so
// a candidate directive — or the first operand of something longer.
//
// The list is the reason `"use strict"` and `"use strict"\n(f)()` mean
// different things: automatic semicolon insertion supplies a terminator only
// where the next token could not continue the expression, so a `(` on the
// following line makes the literal a CALLEE and not a directive. Getting this
// wrong in the permissive direction silently enables strict mode for code
// that never asked for it, which is why the set is spelled out rather than
// approximated by "is it an operator".
bool continuesExpression(TokenKind kind) {
    switch (kind) {
        // Suffixes: a call, a member access, a tagged template.
        case TokenKind::LParen:
        case TokenKind::LBracket:
        case TokenKind::Dot:
        case TokenKind::QuestionDot:
        case TokenKind::TemplateWhole:
        case TokenKind::TemplateHead:
        // Binary and relational operators.
        case TokenKind::Plus:
        case TokenKind::Minus:
        case TokenKind::Star:
        case TokenKind::StarStar:
        case TokenKind::Slash:
        case TokenKind::Percent:
        case TokenKind::Less:
        case TokenKind::Greater:
        case TokenKind::LessEqual:
        case TokenKind::GreaterEqual:
        case TokenKind::EqualEqual:
        case TokenKind::EqualEqualEqual:
        case TokenKind::BangEqual:
        case TokenKind::BangEqualEqual:
        case TokenKind::AmpAmp:
        case TokenKind::PipePipe:
        case TokenKind::QuestionQuestion:
        case TokenKind::Amp:
        case TokenKind::Pipe:
        case TokenKind::Caret:
        case TokenKind::LessLess:
        case TokenKind::GreaterGreater:
        case TokenKind::GreaterGreaterGreater:
        case TokenKind::KwIn:
        case TokenKind::KwInstanceof:
        // The ternary, the comma operator, and every assignment spelling —
        // none of them legal with a string literal on the left, but ASI is
        // defined over what the grammar could PARSE and the error comes later.
        case TokenKind::Question:
        case TokenKind::Comma:
        case TokenKind::Assign:
        case TokenKind::PlusAssign:
        case TokenKind::MinusAssign:
        case TokenKind::StarAssign:
        case TokenKind::SlashAssign:
        case TokenKind::PercentAssign:
        case TokenKind::AmpAssign:
        case TokenKind::PipeAssign:
        case TokenKind::CaretAssign:
        case TokenKind::LessLessAssign:
        case TokenKind::GreaterGreaterAssign:
        case TokenKind::GreaterGreaterGreaterAssign:
        case TokenKind::StarStarAssign:
            return true;
        // `++` and `--` are deliberately absent: they are restricted
        // productions, so a line terminator in front of one ends the statement
        // before it (12.10) rather than making it a postfix operator.
        default:
            return false;
    }
}

// Is this string-literal token the `"use strict"` directive?
//
// The comparison is against the RAW source text, and that is the whole point
// of 11.2.2: the directive counts only if the literal contains no escape
// sequences, so `"use strict"` and `"use \163trict"` are ordinary
// strings that happen to DENOTE "use strict" and select nothing. Comparing
// the decoded value would accept both; comparing the characters between the
// quotes rejects them for free, because an escape is spelled with a
// backslash and the target has none.
bool isUseStrict(const Token& tok) {
    const std::string_view raw = tok.text;
    if (raw.size() < 2) return false;
    return raw.substr(1, raw.size() - 2) == "use strict";
}

}  // namespace

bool Parser::prologueSelectsStrict() const {
    for (size_t ahead = 0;;) {
        const Token& lit = peek(ahead);
        if (lit.kind != TokenKind::StringLiteral) return false;
        const Token& after = peek(ahead + 1);
        // The literal is a whole ExpressionStatement only if a terminator
        // follows it — a written `;`, or a position ASI supplies one at.
        const bool terminated =
            after.kind == TokenKind::Semicolon || after.kind == TokenKind::RBrace ||
            after.kind == TokenKind::EndOfFile ||
            (after.newlineBefore && !continuesExpression(after.kind));
        if (!terminated) return false;
        if (isUseStrict(lit)) return true;
        // Another directive (`"use asm"`, a pragma bronze does not read). The
        // prologue continues through it: 11.2.2 makes it the whole leading run
        // of string-literal statements, so `"a"; "use strict";` is strict.
        ahead += after.kind == TokenKind::Semicolon ? 2 : 1;
    }
}

std::vector<ast::StmtPtr> Parser::parseFunctionBody(bool& outStrict) {
    std::vector<ast::StmtPtr> body;
    StrictScopeGuard guard(*this);
    if (!expect(TokenKind::LBrace, "'{'")) {
        outStrict = strict_;
        return body;
    }
    // Read before a single statement is parsed: the prologue decides which
    // early errors the statements below it are subject to, so a body whose
    // first line is `"use strict"` and whose second is `delete x` has to
    // diagnose the second.
    if (prologueSelectsStrict()) strict_ = true;
    outStrict = strict_;
    while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile) && !diags_.hasErrors()) {
        if (!parseStatement(body)) return body;
    }
    expect(TokenKind::RBrace, "'}' to close block");
    return body;
}

bool Parser::checkStrictBindingName(std::string_view name, Span span, const char* role) {
    if (!strict_) return true;
    if (name == "eval" || name == "arguments") {
        diags_.error(span, "strict mode: '" + std::string(name) + "' may not be bound as a " +
                               role + " (ECMA-262 13.15.1)");
        return false;
    }
    return checkStrictIdentifierReference(name, span);
}

bool Parser::checkStrictIdentifierReference(std::string_view name, Span span) {
    if (!strict_) return true;
    for (const std::string_view reserved : kStrictReservedWords) {
        if (name != reserved) continue;
        diags_.error(span, "strict mode: '" + std::string(name) +
                               "' is a reserved word and may not be used as an identifier "
                               "(ECMA-262 12.7.2)");
        return false;
    }
    return true;
}

bool Parser::checkStrictAssignmentTarget(const ast::Expr& target) {
    if (!strict_) return true;
    const auto* ident = dynamic_cast<const ast::Ident*>(&target);
    if (!ident) return true;
    if (ident->name != "eval" && ident->name != "arguments") return true;
    diags_.error(target.span, "strict mode: '" + ident->name +
                                  "' may not be assigned to (ECMA-262 13.15.1)");
    return false;
}

bool Parser::checkStrictParams(const std::vector<ast::Param>& params, bool bodyStrict) {
    if (!bodyStrict) return true;
    // The body's strictness rather than the enclosing code's: a function that
    // writes its own `"use strict"` is strict, and its parameter list is
    // subject to these rules even though the list was written before the
    // directive that selects them.
    const bool savedStrict = strict_;
    strict_ = true;
    std::vector<std::string> seen;
    bool ok = true;
    for (const auto& p : params) {
        // A pattern binds several names, and all of them count — for the
        // duplicate rule too, since `function f({a}, a)` binds `a` twice.
        std::vector<std::string> names;
        if (p.pattern) {
            names = ast::patternBoundNames(*p.pattern);
        } else if (!p.name.empty()) {
            names.push_back(p.name);
        }
        for (const auto& name : names) {
            if (!checkStrictBindingName(name, p.span, "parameter")) {
                ok = false;
                continue;
            }
            bool duplicate = false;
            for (const auto& prior : seen) duplicate = duplicate || prior == name;
            if (duplicate) {
                diags_.error(p.span, "strict mode: duplicate parameter name '" + name +
                                         "' (ECMA-262 15.2.1)");
                ok = false;
                continue;
            }
            seen.push_back(name);
        }
    }
    strict_ = savedStrict;
    return ok;
}

}  // namespace bronze
