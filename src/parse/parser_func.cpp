// Every form that has a parameter list and a body: function declarations and
// expressions, arrows, and classes — which are the same thing again, with the
// bookkeeping that tells each `super` which class it belongs to.

#include "parse/parser.h"

namespace bronze {

using namespace ast;

StmtPtr Parser::parseFunctionDecl(bool isExported) {
    const Token& kw = advance();  // 'function'
    auto fn = std::make_unique<FunctionDecl>();
    fn->span.begin = kw.span.begin;
    fn->isExported = isExported;

    const Token* name = expect(TokenKind::Identifier, "function name");
    if (!name) return nullptr;
    fn->name = std::string(name->text);

    if (!expect(TokenKind::LParen, "'(' after function name")) return nullptr;
    if (!parseParams(fn->params)) return nullptr;
    if (!expect(TokenKind::RParen, "')' after parameters")) return nullptr;
    if (match(TokenKind::Colon)) fn->returnType = parseTypeAnnotation();

    fn->body = parseBlock();
    if (diags_.hasErrors()) return nullptr;
    fn->span.end = peek().span.begin;
    return fn;
}
// `x => ...` and `(a, b) => ...`. Deciding between an arrow's parameter
// list and a parenthesized expression needs unbounded lookahead in general,
// so this scans forward for the `)` that matches the `(` and asks what
// follows it. Cheaper than parse-and-backtrack, and it cannot half-consume
// the input on a wrong guess.
bool Parser::looksLikeArrow() const {
    if (check(TokenKind::Identifier) && peek(1).kind == TokenKind::Arrow) return true;
    if (!check(TokenKind::LParen)) return false;
    size_t depth = 0;
    for (size_t i = 0;; ++i) {
        const TokenKind kind = peek(i).kind;
        if (kind == TokenKind::EndOfFile) return false;
        if (kind == TokenKind::LParen) ++depth;
        else if (kind == TokenKind::RParen) {
            if (--depth == 0) {
                // A return-type annotation sits between the `)` and the
                // `=>`; the annotation itself is one token today.
                if (peek(i + 1).kind == TokenKind::Arrow) return true;
                return peek(i + 1).kind == TokenKind::Colon &&
                       peek(i + 3).kind == TokenKind::Arrow;
            }
        }
    }
}

ExprPtr Parser::parseArrowFunction() {
    auto fn = std::make_unique<FunctionExpr>();
    fn->isArrow = true;
    fn->span.begin = peek().span.begin;

    if (check(TokenKind::Identifier)) {
        Param p;
        p.name = std::string(advance().text);
        fn->params.push_back(std::move(p));
    } else {
        if (!expect(TokenKind::LParen, "'(' before arrow parameters")) return nullptr;
        if (!parseParams(fn->params)) return nullptr;
        if (!expect(TokenKind::RParen, "')' after arrow parameters")) return nullptr;
        if (match(TokenKind::Colon)) fn->returnType = parseTypeAnnotation();
    }
    if (!expect(TokenKind::Arrow, "'=>' after arrow parameters")) return nullptr;

    if (check(TokenKind::LBrace)) {
        fn->body = parseBlock();
    } else {
        // An expression body IS a return, and is stored as one so that
        // every consumer below — capture analysis, inference, lowering —
        // sees one shape of function body and not two.
        auto value = parseAssign();
        if (!value) return nullptr;
        auto ret = std::make_unique<ReturnStmt>();
        ret->span = value->span;
        ret->value = std::move(value);
        fn->body.push_back(std::move(ret));
    }
    if (diags_.hasErrors()) return nullptr;
    fn->span.end = peek().span.begin;
    return fn;
}
// One parameter list, for every function form there is — declaration,
// expression, arrow and class method. One copy, because four copies of this
// loop are four places for the diagnostics below to drift apart.
//
// Rest, defaults and destructuring are all ES2015 parameter syntax bronze
// has not built; each is named here rather than reported as a missing `)`.
bool Parser::parseParams(std::vector<ast::Param>& out) {
    while (!check(TokenKind::RParen)) {
        if (check(TokenKind::Ellipsis)) {
            error("unsupported construct: rest parameter");
            return false;
        }
        if (check(TokenKind::LBracket) || check(TokenKind::LBrace)) {
            error("unsupported construct: destructuring parameter");
            return false;
        }
        const Token* param = expect(TokenKind::Identifier, "parameter name");
        if (!param) return false;
        Param p;
        p.name = std::string(param->text);
        if (match(TokenKind::Colon)) p.typeAnnotation = parseTypeAnnotation();
        if (check(TokenKind::Assign)) {
            error("unsupported construct: default parameter value");
            return false;
        }
        out.push_back(std::move(p));
        if (!match(TokenKind::Comma)) break;
    }
    return true;
}

// `class Name [extends Base] { members }`. A class introduces no runtime
// concept - it is the constructor function plus its prototype, and lowering
// desugars it into exactly that (docs/0012 decision 5). What the parser
// owes is the shape: which member is the constructor, which are static, and
// which class each `super` in a body belongs to.
//
// Everything ES2015+ puts in a class body that bronze has not built -
// fields, getters and setters, computed keys, generators - is diagnosed by
// name here rather than mis-parsed as a method.
ast::StmtPtr Parser::parseClass() {
    const Token& kw = advance();  // 'class'
    const Token* nameTok = expect(TokenKind::Identifier, "class name");
    if (!nameTok) return nullptr;

    auto cls = std::make_unique<ClassDecl>();
    cls->span.begin = kw.span.begin;
    cls->name = std::string(nameTok->text);

    if (match(TokenKind::KwExtends)) {
        const Token* base = expect(TokenKind::Identifier, "base class name after 'extends'");
        if (!base) return nullptr;
        cls->superName = std::string(base->text);
    }
    if (!expect(TokenKind::LBrace, "'{' to open a class body")) return nullptr;

    // Every `super` inside a method belongs to THIS class, and the parser is
    // the only place that knows which class that is.
    const std::string savedSuper = currentClassSuper_;
    const bool savedInMethod = inClassMethod_;
    currentClassSuper_ = cls->superName;
    inClassMethod_ = true;

    bool ok = true;
    while (!check(TokenKind::RBrace)) {
        if (check(TokenKind::EndOfFile)) {
            error("unterminated class body");
            ok = false;
            break;
        }
        if (match(TokenKind::Semicolon)) continue;  // a stray `;` between members is legal

        ClassMethod member;
        // `static` is not a reserved word: it names a member when something
        // follows it, and is an ordinary method name in `static() {}`.
        if (check(TokenKind::Identifier) && peek().text == "static" &&
            peek(1).kind != TokenKind::LParen) {
            advance();
            member.isStatic = true;
        }
        if (check(TokenKind::Star)) {
            error("unsupported construct: generator method in a class body");
            ok = false;
            break;
        }
        if (check(TokenKind::LBracket)) {
            error("unsupported construct: computed method name in a class body");
            ok = false;
            break;
        }
        if (check(TokenKind::Identifier) && (peek().text == "get" || peek().text == "set") &&
            peek(1).kind == TokenKind::Identifier) {
            error("unsupported construct: class getter or setter");
            ok = false;
            break;
        }
        const Token* memberName = expect(TokenKind::Identifier, "class member name");
        if (!memberName) {
            ok = false;
            break;
        }
        member.name = std::string(memberName->text);
        if (!check(TokenKind::LParen)) {
            // `x = 1;` or `x;` - a field, which runs in the constructor and
            // is not built yet. Named rather than read as a broken method.
            error("unsupported construct: class field (only methods are supported)");
            ok = false;
            break;
        }
        member.isConstructor = !member.isStatic && member.name == "constructor";

        auto fn = std::make_unique<FunctionExpr>();
        fn->span.begin = memberName->span.begin;
        fn->name = cls->name + "." + member.name;
        advance();  // '('
        if (!parseParams(fn->params)) return nullptr;
        if (!expect(TokenKind::RParen, "')' after parameters")) return nullptr;
        if (match(TokenKind::Colon)) fn->returnType = parseTypeAnnotation();
        fn->body = parseBlock();
        if (diags_.hasErrors()) return nullptr;
        fn->span.end = peek().span.begin;
        member.fn = std::move(fn);
        cls->methods.push_back(std::move(member));
    }

    currentClassSuper_ = savedSuper;
    inClassMethod_ = savedInMethod;
    if (!ok) return nullptr;
    if (!expect(TokenKind::RBrace, "'}' to close a class body")) return nullptr;

    // Lowering wants exactly one constructor, always. A base class that
    // writes none gets an empty one, which is what the language says it
    // has. A DERIVED class that writes none is `constructor(...args) {
    // super(...args); }` — rest and spread, neither of which bronze has, and
    // forwarding fewer arguments than were passed would be a wrong answer
    // given quietly. So it is named instead.
    bool hasCtor = false;
    for (const auto& m : cls->methods) hasCtor = hasCtor || m.isConstructor;
    if (!hasCtor) {
        if (!cls->superName.empty()) {
            error("unsupported construct: a derived class with no constructor (write one "
                  "that calls super)");
            return nullptr;
        }
        ClassMethod ctor;
        ctor.name = "constructor";
        ctor.isConstructor = true;
        ctor.fn = std::make_unique<FunctionExpr>();
        ctor.fn->name = cls->name + ".constructor";
        ctor.fn->span = cls->span;
        cls->methods.insert(cls->methods.begin(), std::move(ctor));
    }
    cls->span.end = peek().span.begin;
    return cls;
}

// `super(...)` and `super.m` - only inside a class method, and only in a
// class that has a parent, which is where the name they resolve against
// comes from.
ExprPtr Parser::parseSuper() {
    const Token& kw = advance();  // 'super'
    if (!inClassMethod_) {
        error("unsupported construct: super outside a class method");
        return nullptr;
    }
    if (currentClassSuper_.empty()) {
        error("super in a class with no 'extends'");
        return nullptr;
    }
    if (check(TokenKind::LParen)) {
        advance();
        auto call = std::make_unique<SuperCall>();
        call->span.begin = kw.span.begin;
        call->baseName = currentClassSuper_;
        if (!parseArgumentList(call->args)) return nullptr;
        call->span.end = peek().span.begin;
        return call;
    }
    if (match(TokenKind::Dot)) {
        const Token* member = expect(TokenKind::Identifier, "property name after 'super.'");
        if (!member) return nullptr;
        auto mem = std::make_unique<SuperMember>();
        mem->span = {kw.span.begin, member->span.end};
        mem->baseName = currentClassSuper_;
        mem->property = std::string(member->text);
        return mem;
    }
    error("super must be called or have a property read from it");
    return nullptr;
}

ExprPtr Parser::parseFunctionExpr() {
    const Token& kw = advance();  // 'function'
    auto fn = std::make_unique<FunctionExpr>();
    fn->span.begin = kw.span.begin;

    if (check(TokenKind::Identifier)) {
        fn->name = std::string(advance().text);
    }

    if (!expect(TokenKind::LParen, "'(' after function")) return nullptr;
    if (!parseParams(fn->params)) return nullptr;
    if (!expect(TokenKind::RParen, "')' after parameters")) return nullptr;
    if (match(TokenKind::Colon)) fn->returnType = parseTypeAnnotation();

    fn->body = parseBlock();
    if (diags_.hasErrors()) return nullptr;
    fn->span.end = peek().span.begin;
    return fn;
}

}  // namespace bronze
