// Expressions: precedence-climbing over the binary operators, the unary and
// suffix forms, and the primary operands that are not literals.

#include <charconv>

#include "parse/parser.h"

namespace bronze {

using namespace ast;

namespace {
struct OpInfo {
    BinaryOp op;
    int precedence;
};
const OpInfo* binaryOpInfo(TokenKind kind) {
    static constexpr struct {
        TokenKind kind;
        OpInfo info;
    } kOps[] = {
        {TokenKind::Assign, {BinaryOp::Assign, 0}},
        {TokenKind::PlusAssign, {BinaryOp::PlusAssign, 0}},
        {TokenKind::MinusAssign, {BinaryOp::MinusAssign, 0}},
        {TokenKind::StarAssign, {BinaryOp::StarAssign, 0}},
        {TokenKind::SlashAssign, {BinaryOp::SlashAssign, 0}},
        {TokenKind::PercentAssign, {BinaryOp::PercentAssign, 0}},
        {TokenKind::QuestionQuestion, {BinaryOp::NullishCoalescing, 1}},
        {TokenKind::PipePipe, {BinaryOp::LogicalOr, 2}},
        {TokenKind::AmpAmp, {BinaryOp::LogicalAnd, 3}},
        {TokenKind::EqualEqualEqual, {BinaryOp::StrictEq, 4}},
        {TokenKind::BangEqualEqual, {BinaryOp::StrictNe, 4}},
        {TokenKind::EqualEqual, {BinaryOp::Eq, 4}},
        {TokenKind::BangEqual, {BinaryOp::Ne, 4}},
        {TokenKind::Less, {BinaryOp::Less, 5}},
        {TokenKind::Greater, {BinaryOp::Greater, 5}},
        {TokenKind::LessEqual, {BinaryOp::LessEqual, 5}},
        {TokenKind::GreaterEqual, {BinaryOp::GreaterEqual, 5}},
        {TokenKind::Plus, {BinaryOp::Add, 6}},
        {TokenKind::Minus, {BinaryOp::Sub, 6}},
        {TokenKind::Star, {BinaryOp::Mul, 7}},
        {TokenKind::Slash, {BinaryOp::Div, 7}},
        {TokenKind::Percent, {BinaryOp::Mod, 7}},
    };
    for (const auto& entry : kOps)
        if (entry.kind == kind) return &entry.info;
    return nullptr;
}
}  // namespace
ExprPtr Parser::parseExpr() {
    auto expr = parseBinary(0);
    if (!expr) return nullptr;
    if (match(TokenKind::Question)) {
        auto ternary = std::make_unique<Ternary>();
        ternary->span.begin = expr->span.begin;
        ternary->condition = std::move(expr);
        ternary->thenExpr = parseExpr();
        if (!ternary->thenExpr) return nullptr;
        if (!expect(TokenKind::Colon, "':' in ternary expression")) return nullptr;
        ternary->elseExpr = parseExpr();
        if (!ternary->elseExpr) return nullptr;
        ternary->span.end = ternary->elseExpr->span.end;
        return ternary;
    }
    return expr;
}

ExprPtr Parser::parseBinary(int minPrecedence) {
    auto lhs = parseUnaryPrefix();
    if (!lhs) return nullptr;
    for (;;) {
        const OpInfo* info = binaryOpInfo(peek().kind);
        if (!info || info->precedence < minPrecedence) return lhs;
        advance();
        auto rhs = parseBinary(info->precedence + 1);
        if (!rhs) return nullptr;
        auto bin = std::make_unique<Binary>();
        bin->span = {lhs->span.begin, rhs->span.end};
        bin->op = info->op;
        bin->lhs = std::move(lhs);
        bin->rhs = std::move(rhs);
        lhs = std::move(bin);
    }
}

ExprPtr Parser::parseUnaryPrefix() {
    // Checked at the OPERAND entry point rather than in parseExpr, because
    // an arrow can appear anywhere an operand can — including as the right
    // side of an assignment, which is a binary operator here and so never
    // passes back through parseExpr (`this.get = () => this.count`).
    if (looksLikeArrow()) return parseArrowFunction();
    const Token& t = peek();
    if (check(TokenKind::KwNew)) {
        return parseNew();
    }
    if (check(TokenKind::KwDelete)) {
        // `delete` is what transitions an object to dictionary mode, which
        // is designed but unbuilt (docs/0009 decision 3). The keyword
        // exists purely so the construct can be named: before it, this
        // read as a stray-identifier syntax error naming nothing.
        error("unsupported construct: delete (objects have no dictionary mode yet)");
        return nullptr;
    }
    if (check(TokenKind::KwClass)) {
        error("unsupported construct: class expression");
        return nullptr;
    }
    if (check(TokenKind::Ellipsis)) {
        // A spread in a call, an array literal or an object literal. Named
        // here because every one of those positions parses an expression,
        // so this is the one place they all pass through.
        error("unsupported construct: spread");
        return nullptr;
    }
    if (check(TokenKind::KwSuper)) {
        auto sup = parseSuper();
        if (!sup) return nullptr;
        return parsePostfixOps(std::move(sup));
    }
    if (match(TokenKind::Bang)) {
        auto sub = parseUnaryPrefix();
        if (!sub) return nullptr;
        auto u = std::make_unique<Unary>();
        u->span = {t.span.begin, sub->span.end};
        u->op = UnaryOp::Not;
        u->operand = std::move(sub);
        return u;
    }
    if (match(TokenKind::Minus)) {
        auto sub = parseUnaryPrefix();
        if (!sub) return nullptr;
        auto u = std::make_unique<Unary>();
        u->span = {t.span.begin, sub->span.end};
        u->op = UnaryOp::Negate;
        u->operand = std::move(sub);
        return u;
    }
    if (match(TokenKind::Plus)) {
        auto sub = parseUnaryPrefix();
        if (!sub) return nullptr;
        auto u = std::make_unique<Unary>();
        u->span = {t.span.begin, sub->span.end};
        u->op = UnaryOp::Posate;
        u->operand = std::move(sub);
        return u;
    }
    if (match(TokenKind::PlusPlus)) {
        auto sub = parseUnaryPrefix();
        if (!sub) return nullptr;
        auto u = std::make_unique<Unary>();
        u->span = {t.span.begin, sub->span.end};
        u->op = UnaryOp::PreInc;
        u->operand = std::move(sub);
        return u;
    }
    if (match(TokenKind::MinusMinus)) {
        auto sub = parseUnaryPrefix();
        if (!sub) return nullptr;
        auto u = std::make_unique<Unary>();
        u->span = {t.span.begin, sub->span.end};
        u->op = UnaryOp::PreDec;
        u->operand = std::move(sub);
        return u;
    }
    return parseUnaryPostfix();
}

ExprPtr Parser::parseUnaryPostfix() {
    auto expr = parsePrimary();
    if (!expr) return nullptr;
    return parsePostfixOps(std::move(expr));
}

// The `.p` / `[i]` / `(args)` / `++` / `--` suffix loop, split out from
// parseUnaryPostfix so a `new` expression can be a receiver too: `new
// Point(1, 2).scale(3)` is one member call on a fresh object, and before
// this split parseNew returned straight to the caller, so the `.` after
// the constructor's argument list read as a syntax error.
ExprPtr Parser::parsePostfixOps(ExprPtr expr) {
    for (;;) {
        if (match(TokenKind::Dot)) {
            const Token* member = expect(TokenKind::Identifier, "property name");
            if (!member) return nullptr;
            const auto* baseIdent = dynamic_cast<const ast::Ident*>(expr.get());
            if (baseIdent && baseIdent->name == "console" && member->text == "log") {
                auto ident = std::make_unique<ast::Ident>();
                ident->span = {expr->span.begin, member->span.end};
                ident->name = "console.log";
                expr = std::move(ident);
            } else {
                auto mem = std::make_unique<MemberAccess>();
                mem->span = {expr->span.begin, member->span.end};
                mem->object = std::move(expr);
                mem->property = std::string(member->text);
                expr = std::move(mem);
            }
        } else if (match(TokenKind::LBracket)) {
            auto indexExpr = parseExpr();
            if (!indexExpr) return nullptr;
            if (!expect(TokenKind::RBracket, "']' after index expression")) return nullptr;
            auto idx = std::make_unique<IndexAccess>();
            idx->span = {expr->span.begin, peek().span.begin};
            idx->object = std::move(expr);
            idx->index = std::move(indexExpr);
            expr = std::move(idx);
        } else if (check(TokenKind::LParen)) {
            advance();
            auto call = std::make_unique<Call>();
            call->span.begin = expr->span.begin;
            call->callee = std::move(expr);
            if (!parseArgumentList(call->args)) return nullptr;
            call->span.end = peek().span.begin;
            expr = std::move(call);
        } else if (check(TokenKind::TemplateWhole) || check(TokenKind::TemplateHead)) {
            // `tag`...`` — a template in suffix position is a TAGGED
            // template, which is not the cooked-pieces path of docs/0012
            // decision 1: the tag receives the raw strings and the
            // substitutions as arguments. Named here so it does not read as
            // a missing semicolon.
            error("unsupported construct: tagged template literal");
            return nullptr;
        } else if (check(TokenKind::PlusPlus) && atLineBreak()) {
            // Postfix `++`/`--` are restricted productions: a line terminator
            // before the operator ends the statement, and the `++` belongs to
            // the next one as a PREFIX operator. `let e = d\n++d` leaves e at
            // d's old value and increments d — folding the two lines together
            // would be a silent wrong answer (docs/0014).
            break;
        } else if (match(TokenKind::PlusPlus)) {
            auto u = std::make_unique<Unary>();
            u->span = {expr->span.begin, peek().span.begin};
            u->op = UnaryOp::PostInc;
            u->operand = std::move(expr);
            expr = std::move(u);
        } else if (check(TokenKind::MinusMinus) && atLineBreak()) {
            break;
        } else if (match(TokenKind::MinusMinus)) {
            auto u = std::make_unique<Unary>();
            u->span = {expr->span.begin, peek().span.begin};
            u->op = UnaryOp::PostDec;
            u->operand = std::move(expr);
            expr = std::move(u);
        } else {
            break;
        }
    }
    return expr;
}

bool Parser::parseArgumentList(std::vector<ExprPtr>& args) {
    while (!check(TokenKind::RParen)) {
        auto arg = parseExpr();
        if (!arg) return false;
        args.push_back(std::move(arg));
        if (!match(TokenKind::Comma)) break;
    }
    return expect(TokenKind::RParen, "')' after arguments") != nullptr;
}

// new <Identifier>(args) only for now. Anything fancier after 'new' (member
// expressions, parenthesized callees, nested 'new') is a diagnosed hard error.
ExprPtr Parser::parseNew() {
    const Token& kw = advance();  // 'new'
    if (!check(TokenKind::Identifier)) {
        error("unsupported construct: new with a non-identifier callee");
        return nullptr;
    }
    const Token& name = advance();
    auto ne = std::make_unique<NewExpr>();
    ne->span.begin = kw.span.begin;
    ne->callee = std::string(name.text);
    if (!check(TokenKind::LParen)) {
        error("new requires an argument list");
        return nullptr;
    }
    advance();  // '('
    if (!parseArgumentList(ne->args)) return nullptr;
    ne->span.end = peek().span.begin;
    return parsePostfixOps(std::move(ne));
}
ExprPtr Parser::parsePrimary() {
    const Token& t = peek();
    switch (t.kind) {
        case TokenKind::NumberLiteral: {
            advance();
            auto lit = std::make_unique<NumberLit>();
            lit->span = t.span;
            double value = 0;
            const auto* begin = t.text.data();
            std::from_chars(begin, begin + t.text.size(), value);
            lit->value = value;
            return lit;
        }
        case TokenKind::StringLiteral: {
            advance();
            auto lit = std::make_unique<StringLit>();
            lit->span = t.span;
            lit->value = decodeStringLiteral(t.text.substr(1, t.text.size() - 2), t.span);
            return lit;
        }
        case TokenKind::TemplateWhole: {
            // No substitutions: exactly a string literal, and lowered as
            // one. The token text includes both backticks.
            advance();
            auto lit = std::make_unique<StringLit>();
            lit->span = t.span;
            lit->value = decodeStringLiteral(t.text.substr(1, t.text.size() - 2), t.span);
            return lit;
        }
        case TokenKind::TemplateHead:
            return parseTemplateLiteral();
        case TokenKind::KwTrue: {
            advance();
            auto lit = std::make_unique<BoolLit>();
            lit->span = t.span;
            lit->value = true;
            return lit;
        }
        case TokenKind::KwFalse: {
            advance();
            auto lit = std::make_unique<BoolLit>();
            lit->span = t.span;
            lit->value = false;
            return lit;
        }
        case TokenKind::KwNull: {
            advance();
            auto lit = std::make_unique<NullLit>();
            lit->span = t.span;
            return lit;
        }
        case TokenKind::KwUndefined: {
            advance();
            auto lit = std::make_unique<UndefinedLit>();
            lit->span = t.span;
            return lit;
        }
        case TokenKind::KwThis: {
            advance();
            auto self = std::make_unique<ThisExpr>();
            self->span = t.span;
            return self;
        }
        case TokenKind::Identifier: {
            advance();
            auto ident = std::make_unique<Ident>();
            ident->span = t.span;
            ident->name = std::string(t.text);
            return ident;
        }
        case TokenKind::LParen: {
            advance();
            auto inner = parseExpr();
            if (!inner) return nullptr;
            if (!expect(TokenKind::RParen, "')'")) return nullptr;
            return inner;
        }
        case TokenKind::LBrace:
            return parseObjectLit();
        case TokenKind::LBracket:
            return parseArrayLit();
        case TokenKind::KwFunction:
            return parseFunctionExpr();
        default:
            error("expected expression");
            return nullptr;
    }
}

}  // namespace bronze
