// Expressions: precedence-climbing over the binary operators, the unary and
// suffix forms, and the primary operands that are not literals.

#include "parse/parser.h"

namespace bronze {

using namespace ast;

namespace {

// The binding ladder of ECMA-262 13, loosest rung first. Assignment is NOT
// on it: it is right-associative and its left side is a target rather than
// an operand, so it has its own production (`parseAssign`). Neither is the
// comma operator, which is looser than assignment and lives in `parseExpr`.
enum Precedence : int {
    kPrecNullish = 1,   // ??
    kPrecLogicalOr,     // ||
    kPrecLogicalAnd,    // &&
    kPrecBitOr,         // |
    kPrecBitXor,        // ^
    kPrecBitAnd,        // &
    kPrecEquality,      // == != === !==
    kPrecRelational,    // < > <= >= in instanceof
    kPrecShift,         // << >> >>>
    kPrecAdditive,      // + -
    kPrecMultiplicative,// * / %
    kPrecExponent,      // **
};

struct OpInfo {
    BinaryOp op;
    int precedence;
};
const OpInfo* binaryOpInfo(TokenKind kind) {
    static constexpr struct {
        TokenKind kind;
        OpInfo info;
    } kOps[] = {
        {TokenKind::QuestionQuestion, {BinaryOp::NullishCoalescing, kPrecNullish}},
        {TokenKind::PipePipe, {BinaryOp::LogicalOr, kPrecLogicalOr}},
        {TokenKind::AmpAmp, {BinaryOp::LogicalAnd, kPrecLogicalAnd}},
        {TokenKind::Pipe, {BinaryOp::BitOr, kPrecBitOr}},
        {TokenKind::Caret, {BinaryOp::BitXor, kPrecBitXor}},
        {TokenKind::Amp, {BinaryOp::BitAnd, kPrecBitAnd}},
        {TokenKind::EqualEqualEqual, {BinaryOp::StrictEq, kPrecEquality}},
        {TokenKind::BangEqualEqual, {BinaryOp::StrictNe, kPrecEquality}},
        {TokenKind::EqualEqual, {BinaryOp::Eq, kPrecEquality}},
        {TokenKind::BangEqual, {BinaryOp::Ne, kPrecEquality}},
        {TokenKind::Less, {BinaryOp::Less, kPrecRelational}},
        {TokenKind::Greater, {BinaryOp::Greater, kPrecRelational}},
        {TokenKind::LessEqual, {BinaryOp::LessEqual, kPrecRelational}},
        {TokenKind::GreaterEqual, {BinaryOp::GreaterEqual, kPrecRelational}},
        {TokenKind::KwIn, {BinaryOp::In, kPrecRelational}},
        {TokenKind::KwInstanceof, {BinaryOp::InstanceOf, kPrecRelational}},
        {TokenKind::LessLess, {BinaryOp::Shl, kPrecShift}},
        {TokenKind::GreaterGreater, {BinaryOp::Shr, kPrecShift}},
        {TokenKind::GreaterGreaterGreater, {BinaryOp::UShr, kPrecShift}},
        {TokenKind::Plus, {BinaryOp::Add, kPrecAdditive}},
        {TokenKind::Minus, {BinaryOp::Sub, kPrecAdditive}},
        {TokenKind::Star, {BinaryOp::Mul, kPrecMultiplicative}},
        {TokenKind::Slash, {BinaryOp::Div, kPrecMultiplicative}},
        {TokenKind::Percent, {BinaryOp::Mod, kPrecMultiplicative}},
        {TokenKind::StarStar, {BinaryOp::Exp, kPrecExponent}},
    };
    for (const auto& entry : kOps)
        if (entry.kind == kind) return &entry.info;
    return nullptr;
}

// The assignment operators, which are their own production: right
// associative, and with a TARGET rather than an operand on the left.
bool assignmentOp(TokenKind kind, BinaryOp& out) {
    switch (kind) {
        case TokenKind::Assign: out = BinaryOp::Assign; return true;
        case TokenKind::PlusAssign: out = BinaryOp::PlusAssign; return true;
        case TokenKind::MinusAssign: out = BinaryOp::MinusAssign; return true;
        case TokenKind::StarAssign: out = BinaryOp::StarAssign; return true;
        case TokenKind::SlashAssign: out = BinaryOp::SlashAssign; return true;
        case TokenKind::PercentAssign: out = BinaryOp::PercentAssign; return true;
        case TokenKind::AmpAssign: out = BinaryOp::AmpAssign; return true;
        case TokenKind::PipeAssign: out = BinaryOp::PipeAssign; return true;
        case TokenKind::CaretAssign: out = BinaryOp::CaretAssign; return true;
        case TokenKind::LessLessAssign: out = BinaryOp::ShlAssign; return true;
        case TokenKind::GreaterGreaterAssign: out = BinaryOp::ShrAssign; return true;
        case TokenKind::GreaterGreaterGreaterAssign: out = BinaryOp::UShrAssign; return true;
        case TokenKind::StarStarAssign: out = BinaryOp::ExpAssign; return true;
        default: return false;
    }
}

// `??` and the two logical operators may not be mixed without parentheses:
// ECMA-262 states CoalesceExpression over BitwiseORExpression operands, so
// neither `a ?? b || c` nor `a || b ?? c` is a program. The check is over
// the *unparenthesized* form, which is the only reason `Expr` records that.
bool mixesWith(const Expr* e, BinaryOp a, BinaryOp b) {
    if (e == nullptr || e->parenthesized) return false;
    const auto* bin = dynamic_cast<const Binary*>(e);
    return bin != nullptr && (bin->op == a || bin->op == b);
}

}  // namespace

// *Expression*: one or more AssignmentExpressions separated by the comma
// operator, which evaluates its left operand for effect and yields its
// right. Left-associative, and looser than everything else there is.
ExprPtr Parser::parseExpr() {
    auto expr = parseAssign();
    if (!expr) return nullptr;
    while (check(TokenKind::Comma)) {
        advance();
        auto rhs = parseAssign();
        if (!rhs) return nullptr;
        auto bin = std::make_unique<Binary>();
        bin->span = {expr->span.begin, rhs->span.end};
        bin->op = BinaryOp::Comma;
        bin->lhs = std::move(expr);
        bin->rhs = std::move(rhs);
        expr = std::move(bin);
    }
    return expr;
}

// *AssignmentExpression*. The target is parsed as an ordinary conditional
// expression and validated in lowering, which is where the member and index
// forms are already distinguished; the right side recurses through here, so
// `a = b = c` is `a = (b = c)` and `x = cond ? p : q` assigns the CONDITIONAL
// rather than testing the assignment.
ExprPtr Parser::parseAssign() {
    if (looksLikeArrow()) return parseArrowFunction();
    auto lhs = parseConditional();
    if (!lhs) return nullptr;
    BinaryOp op{};
    if (!assignmentOp(peek().kind, op)) return lhs;
    // `a?.b = 1` is an early error (ECMA-262 13.3.9 / 13.15.1): the left side
    // of an assignment is a target, and a chain that may decide not to
    // evaluate is not one. `(a?.b).c = 1` is legal and reaches here with a
    // parenthesized base, which `containsOptionalLink` stops at.
    if (ast::containsOptionalLink(*lhs)) {
        error("an optional chain is not a valid assignment target");
        return nullptr;
    }
    // `[a, b] = pair` and `({ x } = o)`. Nothing before the `=` distinguishes
    // the pattern from the literal that covers it, which is why ECMA-262
    // 13.15.5 refines it exactly here — at the token that reveals which one
    // the source meant. A COMPOUND operator has no such reading: `[a] += x`
    // would have to read the pattern as a value first.
    const bool literalTarget =
        dynamic_cast<ArrayLit*>(lhs.get()) != nullptr || dynamic_cast<ObjectLit*>(lhs.get()) != nullptr;
    if (literalTarget) {
        if (op != BinaryOp::Assign) {
            error("a destructuring pattern may only be the target of '='");
            return nullptr;
        }
        const uint32_t begin = lhs->span.begin;
        advance();
        auto pattern = patternFromLiteral(std::move(lhs));
        if (!pattern) return nullptr;
        auto rhs = parseAssign();
        if (!rhs) return nullptr;
        auto node = std::make_unique<DestructuringAssign>();
        node->span = {begin, rhs->span.end};
        node->pattern = std::move(pattern);
        node->value = std::move(rhs);
        return node;
    }
    advance();
    auto rhs = parseAssign();
    if (!rhs) return nullptr;
    auto bin = std::make_unique<Binary>();
    bin->span = {lhs->span.begin, rhs->span.end};
    bin->op = op;
    bin->lhs = std::move(lhs);
    bin->rhs = std::move(rhs);
    return bin;
}

// *ConditionalExpression*. Both arms are AssignmentExpressions, so a comma
// inside one belongs to the enclosing list and not to the conditional.
ExprPtr Parser::parseConditional() {
    auto expr = parseBinary(kPrecNullish);
    if (!expr) return nullptr;
    if (!match(TokenKind::Question)) return expr;
    auto ternary = std::make_unique<Ternary>();
    ternary->span.begin = expr->span.begin;
    ternary->condition = std::move(expr);
    ternary->thenExpr = parseAssign();
    if (!ternary->thenExpr) return nullptr;
    if (!expect(TokenKind::Colon, "':' in ternary expression")) return nullptr;
    ternary->elseExpr = parseAssign();
    if (!ternary->elseExpr) return nullptr;
    ternary->span.end = ternary->elseExpr->span.end;
    return ternary;
}

ExprPtr Parser::parseBinary(int minPrecedence) {
    auto lhs = parseUnaryPrefix();
    if (!lhs) return nullptr;
    bool lhsIsUnary = lastOperandIsUnary_;
    for (;;) {
        const OpInfo* info = binaryOpInfo(peek().kind);
        if (!info || info->precedence < minPrecedence) return lhs;
        if (info->op == BinaryOp::Exp && lhsIsUnary) {
            // Diagnosed rather than resolved: ECMA-262 refuses to pick a
            // reading for `-2 ** 2`, because both `(-2) ** 2` and `-(2 ** 2)`
            // are things a programmer plausibly meant and they differ.
            error("'**' cannot have an unparenthesized unary operand on its left "
                  "(write (-x) ** y or -(x ** y))");
            return nullptr;
        }
        if (info->op == BinaryOp::NullishCoalescing &&
            mixesWith(lhs.get(), BinaryOp::LogicalAnd, BinaryOp::LogicalOr)) {
            error("'??' cannot be mixed with '&&' or '||' without parentheses");
            return nullptr;
        }
        if ((info->op == BinaryOp::LogicalAnd || info->op == BinaryOp::LogicalOr) &&
            mixesWith(lhs.get(), BinaryOp::NullishCoalescing,
                      BinaryOp::NullishCoalescing)) {
            error("'??' cannot be mixed with '&&' or '||' without parentheses");
            return nullptr;
        }
        advance();
        // `**` is the one right-associative binary operator, so its right
        // operand is parsed at its OWN precedence rather than one above it:
        // `2 ** 3 ** 2` is `2 ** (3 ** 2)`.
        const int rhsMin =
            info->op == BinaryOp::Exp ? info->precedence : info->precedence + 1;
        auto rhs = parseBinary(rhsMin);
        if (!rhs) return nullptr;
        if (info->op == BinaryOp::NullishCoalescing &&
            mixesWith(rhs.get(), BinaryOp::LogicalAnd, BinaryOp::LogicalOr)) {
            error("'??' cannot be mixed with '&&' or '||' without parentheses");
            return nullptr;
        }
        auto bin = std::make_unique<Binary>();
        bin->span = {lhs->span.begin, rhs->span.end};
        bin->op = info->op;
        bin->lhs = std::move(lhs);
        bin->rhs = std::move(rhs);
        lhs = std::move(bin);
        lhsIsUnary = false;
    }
}

// *UnaryExpression*, plus the update forms below it. Every path sets
// `lastOperandIsUnary_` on its way out — never on the way in, because
// anything parsed BELOW this call (a parenthesized `-2`, an argument list
// holding one) would leave the flag set from an operand that is not this
// one. It is read by `parseBinary` immediately after this returns, to decide
// whether a `**` has a legal left side: an UpdateExpression (`++a`, `a--`,
// a call, a literal, a parenthesized anything) may be one and a
// UnaryExpression may not.
ExprPtr Parser::parseUnaryPrefix() {
    const Token& t = peek();
    if (check(TokenKind::KwNew)) {
        auto ne = parseNew();
        lastOperandIsUnary_ = false;
        return ne;
    }
    if (check(TokenKind::KwDelete)) {
        // A *UnaryExpression* operand, like every other prefix operator —
        // but one whose value is never taken: `delete o.k` reads no
        // property. Lowering dispatches on the operand's node kind
        // (docs/0019 decision 2), so the parser's only job is to build it.
        const Token& kw = advance();
        auto operand = parseUnaryPrefix();
        if (!operand) return nullptr;
        auto del = std::make_unique<Unary>();
        del->span = {kw.span.begin, operand->span.end};
        del->op = UnaryOp::Delete;
        del->operand = std::move(operand);
        lastOperandIsUnary_ = true;
        return del;
    }
    if (check(TokenKind::KwClass)) {
        error("unsupported construct: class expression");
        return nullptr;
    }
    if (check(TokenKind::Ellipsis)) {
        // The three list positions that admit a spread parse it themselves,
        // because what it means there is "contribute several elements" and
        // only the list can do that. Reaching the operand parser means the
        // `...` is somewhere no list can absorb it.
        error("'...' is only allowed in an argument list, an array literal, an object "
              "literal, or a binding pattern");
        return nullptr;
    }
    if (check(TokenKind::KwSuper)) {
        auto sup = parseSuper();
        if (!sup) return nullptr;
        auto suffixed = parsePostfixOps(std::move(sup));
        lastOperandIsUnary_ = false;
        return suffixed;
    }
    // The six prefix operators that produce a *UnaryExpression*. One loop
    // over a table rather than six copies of the same five lines, which is
    // also what makes "this operand is a unary expression" one assignment
    // instead of six.
    static constexpr struct {
        TokenKind kind;
        UnaryOp op;
    } kPrefixOps[] = {
        {TokenKind::Bang, UnaryOp::Not},     {TokenKind::Minus, UnaryOp::Negate},
        {TokenKind::Plus, UnaryOp::Posate},  {TokenKind::Tilde, UnaryOp::BitNot},
        {TokenKind::KwTypeof, UnaryOp::TypeOf}, {TokenKind::KwVoid, UnaryOp::Void},
    };
    for (const auto& entry : kPrefixOps) {
        if (!check(entry.kind)) continue;
        advance();
        auto sub = parseUnaryPrefix();
        if (!sub) return nullptr;
        auto u = std::make_unique<Unary>();
        u->span = {t.span.begin, sub->span.end};
        u->op = entry.op;
        u->operand = std::move(sub);
        lastOperandIsUnary_ = true;
        return u;
    }
    if (match(TokenKind::PlusPlus)) {
        auto sub = parseUnaryPrefix();
        if (!sub) return nullptr;
        auto u = std::make_unique<Unary>();
        u->span = {t.span.begin, sub->span.end};
        u->op = UnaryOp::PreInc;
        u->operand = std::move(sub);
        lastOperandIsUnary_ = false;
        return u;
    }
    if (match(TokenKind::MinusMinus)) {
        auto sub = parseUnaryPrefix();
        if (!sub) return nullptr;
        auto u = std::make_unique<Unary>();
        u->span = {t.span.begin, sub->span.end};
        u->op = UnaryOp::PreDec;
        u->operand = std::move(sub);
        lastOperandIsUnary_ = false;
        return u;
    }
    auto operandExpr = parseUnaryPostfix();
    lastOperandIsUnary_ = false;
    return operandExpr;
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
        if (check(TokenKind::QuestionDot)) {
            // `?.` is one punctuator in front of THREE productions — `?.name`,
            // `?.[i]` and `?.(args)` — so which link follows is decided here
            // and not by a second `.`/`[`/`(` branch below. The node it builds
            // is the ordinary one with `optional` set: what differs is when the
            // link runs, not what it computes (docs/0018 decision 4).
            advance();
            if (check(TokenKind::LBracket)) {
                advance();
                auto indexExpr = parseExpr();
                if (!indexExpr) return nullptr;
                if (!expect(TokenKind::RBracket, "']' after index expression")) return nullptr;
                auto idx = std::make_unique<IndexAccess>();
                idx->span = {expr->span.begin, peek().span.begin};
                idx->object = std::move(expr);
                idx->index = std::move(indexExpr);
                idx->optional = true;
                expr = std::move(idx);
                continue;
            }
            if (check(TokenKind::LParen)) {
                advance();
                auto call = std::make_unique<Call>();
                call->span.begin = expr->span.begin;
                call->callee = std::move(expr);
                call->optional = true;
                if (!parseArgumentList(call->args)) return nullptr;
                call->span.end = peek().span.begin;
                expr = std::move(call);
                continue;
            }
            if (check(TokenKind::TemplateWhole) || check(TokenKind::TemplateHead)) {
                // ECMA-262 13.3.9 has no OptionalChain production for a
                // template, precisely so that `a?.b`...`` cannot silently mean
                // "tag it, unless a is nullish".
                error("a tagged template may not be part of an optional chain");
                return nullptr;
            }
            const Token* member = expectPropertyName("property name after '?.'");
            if (!member) return nullptr;
            auto mem = std::make_unique<MemberAccess>();
            mem->span = {expr->span.begin, member->span.end};
            mem->object = std::move(expr);
            mem->property = std::string(member->text);
            mem->optional = true;
            expr = std::move(mem);
        } else if (match(TokenKind::Dot)) {
            const Token* member = expectPropertyName("property name");
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
        } else if ((check(TokenKind::PlusPlus) || check(TokenKind::MinusMinus)) &&
                   !atLineBreak() && ast::containsOptionalLink(*expr)) {
            // An UpdateExpression's operand must be a valid assignment target,
            // and ECMA-262 13.3.9 makes an OptionalChain none: `a?.b++` would
            // have to write a property the chain may have decided not to read.
            error("an optional chain is not a valid target for '++' or '--'");
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
        // An *AssignmentExpression*, not an Expression: the commas here
        // separate arguments, and a comma OPERATOR would silently turn
        // `f(a, b)` into a one-argument call (docs/0015 decision 7).
        //
        // A `...` argument contributes as many arguments as its source has,
        // which is a fact about the LIST and not about the expression, so the
        // list is what parses it.
        if (check(TokenKind::Ellipsis)) {
            const Token& dots = advance();
            auto spread = std::make_unique<SpreadElement>();
            spread->argument = parseAssign();
            if (!spread->argument) return false;
            spread->span = {dots.span.begin, spread->argument->span.end};
            args.push_back(std::move(spread));
        } else {
            auto arg = parseAssign();
            if (!arg) return false;
            args.push_back(std::move(arg));
        }
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
            if (!decodeNumericLiteral(t.text, t.span, lit->value)) return nullptr;
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
            // The parentheses are not a node — they change nothing about
            // what the expression computes — but two of the spec's rules are
            // stated over the unparenthesized form, so the fact is recorded
            // on the expression they wrapped (see ast::Expr).
            inner->parenthesized = true;
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
