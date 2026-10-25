// The lookaheads that decide whether tokens in an EXPRESSION are typed
// syntax: `f<T>(x)` is a call with type arguments only if the `<…>` reads as
// a type argument list and a call follows it, and `(a: T): R => …` or
// `<T>(x) => x` is an arrow only if the whole head reads as one. Both parse
// ahead speculatively and give back everything they read when the answer is
// no.

#include "parse/parser.h"

namespace bronze {

using namespace ast;

bool Parser::tryConsumeTypeArgsForCall() {
    if (!check(TokenKind::Less)) return false;
    const SpeculationMark mark = markSpeculation();

    ++speculativeDepth_;
    advance();  // '<'
    bool ok = false;
    bool sawAny = false;
    while (!check(TokenKind::EndOfFile)) {
        if (sawAny && matchTypeGreater()) {
            ok = true;
            break;
        }
        auto arg = parseTypeAnnotation(false);
        if (!arg || diags_.size() > mark.diags) {
            ok = false;
            break;
        }
        sawAny = true;
        if (match(TokenKind::Comma)) continue;
        if (matchTypeGreater()) {
            ok = true;
        }
        break;
    }
    --speculativeDepth_;

    // Type arguments on a call / new / tagged-template MUST be immediately followed by
    // `(`, TemplateWhole, or TemplateHead (and for `(`, on the same line or valid call).
    const bool followedByCallOrTemplate =
        ok && diags_.size() == mark.diags &&
        (check(TokenKind::LParen) || check(TokenKind::TemplateWhole) ||
         check(TokenKind::TemplateHead));

    if (!followedByCallOrTemplate) {
        rollbackSpeculation(mark);
        return false;
    }

    if (speculativeDepth_ == 0) {
        tokenEditsLog_.clear();
        typeDeclUndo_.clear();
    }
    return true;
}

bool Parser::looksLikeArrowFrom(size_t offset) const {
    const TokenKind first = peek(offset).kind;
    if ((first == TokenKind::Identifier || first == TokenKind::KwOf) &&
        peek(offset + 1).kind == TokenKind::Arrow && !peek(offset + 1).newlineBefore) {
        return true;
    }
    // `<T>(x) => x` is a generic arrow only in TypeScript; no JavaScript
    // expression starts with `<`.
    if (first != TokenKind::LParen && !(first == TokenKind::Less && typescript_)) return false;

    auto* self = const_cast<Parser*>(this);
    const SpeculationMark mark = self->markSpeculation();

    self->pos_ = mark.pos + offset;
    ++self->speculativeDepth_;

    bool isArrow = false;
    bool ok = true;
    // The arrow's type parameters, when it has some, are declared in a scope
    // of their own, as parseArrowFunction will declare them for real.
    TypeScopeGuard scope(*self);
    if (self->check(TokenKind::Less)) {
        ok = self->parseTypeParams() && self->diags_.size() == mark.diags;
    }
    if (ok && self->match(TokenKind::LParen)) {
        // Track whether parameters have explicit TS/param syntax (empty `()`, `:`, `?`, `=`, `...`, pattern)
        bool hasExplicitParamSyntax = self->check(TokenKind::RParen);
        const size_t paramsStart = self->pos_;
        std::vector<ast::Param> dummyParams;
        if (self->parseParams(dummyParams) && self->diags_.size() == mark.diags &&
            self->match(TokenKind::RParen)) {
            for (size_t k = paramsStart; k + 1 < self->pos_; ++k) {
                const TokenKind tk = self->tokens_[k].kind;
                if (tk == TokenKind::Colon || tk == TokenKind::Question ||
                    tk == TokenKind::Assign || tk == TokenKind::Ellipsis ||
                    tk == TokenKind::LBracket || tk == TokenKind::LBrace) {
                    hasExplicitParamSyntax = true;
                    break;
                }
            }
            if (self->check(TokenKind::Arrow) && !self->atLineBreak()) {
                isArrow = true;
            } else if (self->match(TokenKind::Colon)) {
                // In `cond ? (a) : b => c`, `(a)` is a parenthesized identifier in the ternary's
                // then-branch and `:` is the ternary's colon, unless `(a)` has explicit param syntax.
                // In a `.js` file a return type there is never read: every
                // `c ? (…) : …` is a JavaScript conditional, whatever the
                // parentheses hold (`c ? (a = 1) : b => b` included).
                if (!(inConditionalThen_ && first == TokenKind::LParen &&
                      (!hasExplicitParamSyntax || !typescript_))) {
                    auto ret = self->parseTypeAnnotation(/*inArrowReturn=*/true);
                    if (ret && self->diags_.size() == mark.diags &&
                        self->check(TokenKind::Arrow) && !self->atLineBreak()) {
                        isArrow = true;
                    }
                }
            }
        }
    }

    --self->speculativeDepth_;
    self->rollbackSpeculation(mark);
    return isArrow;
}

}  // namespace bronze
