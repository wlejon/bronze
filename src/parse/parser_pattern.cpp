// Binding patterns: the `[a, b]` and `{ x, y: z }` forms wherever the grammar
// spells a binding, and the cover-grammar refinement that turns an array or
// object LITERAL into the pattern a destructuring assignment meant.
//
// A pattern is written the same way in every position, so there is one parser
// for it and not four; what differs between the positions is only what the
// caller does with the names it binds.

#include "parse/parser.h"

namespace bronze {

using namespace ast;

PatternPtr Parser::parsePattern() {
    if (check(TokenKind::LBracket)) return parseArrayPattern();
    if (check(TokenKind::LBrace)) return parseObjectPattern();
    error("expected a binding pattern: '[' or '{'");
    return nullptr;
}

// The target half of one element: a name, or a nested pattern. Nesting is why
// this is shared — `const [{ a }, [b]] = pairs` reaches it three times.
bool Parser::parsePatternTarget(PatternElement& elem) {
    if (check(TokenKind::LBracket) || check(TokenKind::LBrace)) {
        elem.pattern = parsePattern();
        return elem.pattern != nullptr;
    }
    const Token* name = expect(TokenKind::Identifier, "a binding name in a pattern");
    if (!name) return false;
    if (!checkStrictBindingName(name->text, name->span, "binding")) return false;
    elem.name = std::string(name->text);
    return true;
}

// `[a, b = 1, [c], ...rest]`. Read by POSITION, which is why no element here
// carries a key.
//
// An ELISION — `[, x]` — is a named hard error rather than a skipped
// position. It is the one pattern form whose meaning is carried entirely by
// absent text, and bronze has no sparse arrays for the array literal spelling
// of it either; supporting one and not the other is how the two would drift.
PatternPtr Parser::parseArrayPattern() {
    const Token& open = advance();  // '['
    auto pat = std::make_unique<BindingPattern>();
    pat->isObject = false;
    pat->span.begin = open.span.begin;

    while (!check(TokenKind::RBracket)) {
        if (check(TokenKind::EndOfFile)) {
            error("unterminated array pattern");
            return nullptr;
        }
        if (check(TokenKind::Comma)) {
            error("unsupported construct: an elision (a hole) in an array pattern");
            return nullptr;
        }
        PatternElement elem;
        elem.span.begin = peek().span.begin;
        const bool isRest = match(TokenKind::Ellipsis);
        elem.isRest = isRest;
        if (!parsePatternTarget(elem)) return nullptr;
        if (check(TokenKind::Assign)) {
            if (isRest) {
                error("a rest element may not have a default value");
                return nullptr;
            }
            advance();
            elem.defaultValue = parseAssign();
            if (!elem.defaultValue) return nullptr;
        }
        elem.span.end = peek().span.begin;
        pat->elements.push_back(std::move(elem));

        if (isRest) {
            // `...rest` takes everything that is left, so nothing can follow
            // it — including the trailing comma an ordinary element allows.
            if (!check(TokenKind::RBracket)) {
                error("a rest element must be the last element of an array pattern");
                return nullptr;
            }
            break;
        }
        if (!match(TokenKind::Comma)) break;
    }

    if (!expect(TokenKind::RBracket, "']' to close an array pattern")) return nullptr;
    pat->span.end = peek().span.begin;
    return pat;
}

// `{ x, y: renamed, z = 5, [k]: c, ...others }`. Read by KEY, so every
// element but the rest carries one — the shorthand `{ x }` being the case
// where the key and the bound name are the same token, built once so the two
// cannot disagree about which property is meant.
PatternPtr Parser::parseObjectPattern() {
    const Token& open = advance();  // '{'
    auto pat = std::make_unique<BindingPattern>();
    pat->isObject = true;
    pat->span.begin = open.span.begin;

    while (!check(TokenKind::RBrace)) {
        if (check(TokenKind::EndOfFile)) {
            error("unterminated object pattern");
            return nullptr;
        }
        PatternElement elem;
        elem.span.begin = peek().span.begin;
        const bool isRest = match(TokenKind::Ellipsis);
        elem.isRest = isRest;

        if (isRest) {
            // `{ ...others }` binds a fresh OBJECT of what is left, so its
            // target is a name and never a nested pattern: there is no
            // property to read it from.
            const Token* name = expect(TokenKind::Identifier, "a name after '...' in an object pattern");
            if (!name) return nullptr;
            elem.name = std::string(name->text);
        } else if (check(TokenKind::LBracket)) {
            advance();
            elem.keyExpr = parseAssign();
            if (!elem.keyExpr) return nullptr;
            if (!expect(TokenKind::RBracket, "']' after a computed pattern key")) return nullptr;
            if (!expect(TokenKind::Colon, "':' after a computed pattern key")) return nullptr;
            if (!parsePatternTarget(elem)) return nullptr;
        } else if (check(TokenKind::StringLiteral)) {
            const Token& keyTok = advance();
            elem.key = decodeStringLiteral(keyTok.text.substr(1, keyTok.text.size() - 2),
                                           keyTok.span);
            if (!expect(TokenKind::Colon, "':' after a property name in an object pattern")) {
                return nullptr;
            }
            if (!parsePatternTarget(elem)) return nullptr;
        } else {
            const Token* keyTok = expect(TokenKind::Identifier, "a property name in an object pattern");
            if (!keyTok) return nullptr;
            elem.key = std::string(keyTok->text);
            if (match(TokenKind::Colon)) {
                if (!parsePatternTarget(elem)) return nullptr;
            } else {
                elem.name = elem.key;  // shorthand: one token, one meaning
            }
        }

        if (check(TokenKind::Assign)) {
            if (isRest) {
                error("a rest property may not have a default value");
                return nullptr;
            }
            advance();
            elem.defaultValue = parseAssign();
            if (!elem.defaultValue) return nullptr;
        }
        elem.span.end = peek().span.begin;
        pat->elements.push_back(std::move(elem));

        if (isRest) {
            if (!check(TokenKind::RBrace)) {
                error("a rest property must be the last element of an object pattern");
                return nullptr;
            }
            break;
        }
        if (!match(TokenKind::Comma)) break;
    }

    if (!expect(TokenKind::RBrace, "'}' to close an object pattern")) return nullptr;
    pat->span.end = peek().span.begin;
    return pat;
}

// ECMA-262 13.15.5. `[a, b] = pair` parses as an ArrayLiteral because nothing
// before the `=` distinguishes it from one; the refinement is the spec's own
// answer, and it happens HERE rather than in lowering because the parser owns
// the nodes and can move them across.
PatternPtr Parser::patternFromLiteral(ExprPtr expr) {
    auto pat = std::make_unique<BindingPattern>();
    pat->span = expr->span;

    // The target half of one element, shared by both literal shapes: a name,
    // a nested literal (refined in turn), or a diagnosed non-target.
    const auto takeTarget = [&](ExprPtr value, PatternElement& elem) -> bool {
        if (auto* ident = dynamic_cast<Ident*>(value.get())) {
            elem.name = ident->name;
            return true;
        }
        if (dynamic_cast<ArrayLit*>(value.get()) || dynamic_cast<ObjectLit*>(value.get())) {
            elem.pattern = patternFromLiteral(std::move(value));
            return elem.pattern != nullptr;
        }
        // `[o.a] = xs` and `({ k: o[i] } = src)`. 13.15.5.2 calls these a
        // DestructuringAssignmentTarget that is neither literal, and evaluates
        // the REFERENCE before the element is read — which is why the node is
        // carried whole to lowering rather than flattened to a name here.
        // An optional link (`a?.b = v`) is not a valid target in any position
        // (13.3.9), so it is rejected with the rest.
        if (auto* mem = dynamic_cast<MemberAccess*>(value.get()); mem && !mem->optional) {
            elem.target = std::move(value);
            return true;
        }
        if (auto* idx = dynamic_cast<IndexAccess*>(value.get()); idx && !idx->optional) {
            elem.target = std::move(value);
            return true;
        }
        // Not "unsupported construct" any more, because what is left here is
        // not a construct bronze has yet to build: 13.15.1 makes a target that
        // is not a simple assignment target — a call, a literal, an operator
        // — an early SyntaxError, and 13.3.9 says the same of an optional
        // chain. Every target the language admits is handled above.
        diags_.error(value->span,
                     "a destructuring assignment target must be a name, a property reference "
                     "or a nested pattern (ECMA-262 13.15.1)");
        return false;
    };

    // `t = d` in target position is a target with a default, not an
    // assignment to evaluate — the same text means both, and only the
    // enclosing pattern decides which.
    const auto takeTargetAndDefault = [&](ExprPtr value, PatternElement& elem) -> bool {
        if (auto* bin = dynamic_cast<Binary*>(value.get())) {
            if (bin->op == BinaryOp::Assign) {
                elem.defaultValue = std::move(bin->rhs);
                return takeTarget(std::move(bin->lhs), elem);
            }
        }
        return takeTarget(std::move(value), elem);
    };

    if (auto* arr = dynamic_cast<ArrayLit*>(expr.get())) {
        pat->isObject = false;
        for (size_t i = 0; i < arr->elements.size(); ++i) {
            PatternElement elem;
            elem.span = arr->elements[i]->span;
            if (auto* spread = dynamic_cast<SpreadElement*>(arr->elements[i].get())) {
                elem.isRest = true;
                if (i + 1 != arr->elements.size()) {
                    diags_.error(elem.span,
                                 "a rest element must be the last element of an array pattern");
                    return nullptr;
                }
                if (!takeTarget(std::move(spread->argument), elem)) return nullptr;
            } else if (!takeTargetAndDefault(std::move(arr->elements[i]), elem)) {
                return nullptr;
            }
            pat->elements.push_back(std::move(elem));
        }
        return pat;
    }

    auto* obj = dynamic_cast<ObjectLit*>(expr.get());
    if (!obj) {
        diags_.error(expr->span, "internal: refining a pattern from something that is not a "
                                 "literal");
        return nullptr;
    }
    pat->isObject = true;
    for (size_t i = 0; i < obj->props.size(); ++i) {
        auto& prop = obj->props[i];
        PatternElement elem;
        elem.span = prop.value ? prop.value->span : pat->span;
        if (auto* spread = dynamic_cast<SpreadElement*>(prop.value.get())) {
            elem.isRest = true;
            if (i + 1 != obj->props.size()) {
                diags_.error(elem.span,
                             "a rest element must be the last element of an object pattern");
                return nullptr;
            }
            auto* ident = dynamic_cast<Ident*>(spread->argument.get());
            if (!ident) {
                diags_.error(elem.span, "the target of '...' in an object pattern must be a name");
                return nullptr;
            }
            elem.name = ident->name;
            pat->elements.push_back(std::move(elem));
            continue;
        }
        elem.key = prop.key;
        elem.keyExpr = std::move(prop.keyExpr);
        if (!takeTargetAndDefault(std::move(prop.value), elem)) return nullptr;
        pat->elements.push_back(std::move(elem));
    }
    return pat;
}

// Where a binding target ENDS, counted in tokens. `for (const [k, v] of m)`
// and `for (const k in m)` are told apart by the token after the target, and
// a pattern's length is unbounded, so the header cannot use fixed lookahead.
size_t Parser::skipBindingTarget(size_t at) const {
    const TokenKind first = peek(at).kind;
    if (first != TokenKind::LBracket && first != TokenKind::LBrace) {
        // A plain name, optionally annotated. The annotation is one token.
        size_t next = at + 1;
        if (peek(next).kind == TokenKind::Colon) next += 2;
        return next;
    }
    size_t depth = 0;
    for (size_t i = at;; ++i) {
        const TokenKind kind = peek(i).kind;
        if (kind == TokenKind::EndOfFile) return i;
        if (kind == TokenKind::LBracket || kind == TokenKind::LBrace ||
            kind == TokenKind::LParen) {
            ++depth;
        } else if (kind == TokenKind::RBracket || kind == TokenKind::RBrace ||
                   kind == TokenKind::RParen) {
            if (--depth == 0) return i + 1;
        }
    }
}

}  // namespace bronze
