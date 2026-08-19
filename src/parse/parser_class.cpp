// Class bodies: which member is the constructor, which are static, which class
// each `super` belongs to — and PRIVATE elements, which are the one part of a
// class that is not sugar over an object and a prototype.
//
// A private name is not a property key. `#x` names an element of the object's
// [[PrivateElements]] list (ECMA-262 6.2.12), reachable only from inside the
// class body that declared it, invisible to every enumeration there is, and
// per class EVALUATION rather than per source position. That last point is why
// the parser's whole job here is naming and scope: it decides which `#x` a
// mention refers to, and refuses the ones that refer to nothing. What a
// private name IS at run time is lowering's answer (lower_private.cpp).
//
// 15.7.1's early errors are stated over the WHOLE ClassBody, not over one
// element, so both of them need the body's private names before the first
// member is parsed — a method written above a field may mention it. Hence the
// token pre-scan below: it is not an optimisation, it is the only order in
// which the rule can be checked in one pass.

#include <string>
#include <vector>

#include "parse/parser.h"

namespace bronze {

using namespace ast;

// A `#x` at the class body's own brace depth is a declaration: the two
// reference forms are `.#x` (13.3.2) and `#x in o` (13.10.1), and neither can
// be spelled without the token that gives it away sitting right beside the
// name. Anything at a deeper brace depth belongs to a method body, a field
// initializer or a static block — including a NESTED class body, whose own
// scan will find its own declarations.
//
// Template substitutions cannot confuse the depth count: the lexer keeps
// `${`/`}` inside the template tokens, so no brace of one is a token here.
void Parser::scanPrivateDeclarations(size_t braceIndex, PrivateNameScope& scope) const {
    size_t depth = 0;
    for (size_t i = braceIndex; i < tokens_.size(); ++i) {
        const TokenKind kind = tokens_[i].kind;
        if (kind == TokenKind::EndOfFile) return;
        if (kind == TokenKind::LBrace) {
            ++depth;
            continue;
        }
        if (kind == TokenKind::RBrace) {
            if (--depth == 0) return;
            continue;
        }
        if (kind != TokenKind::PrivateName || depth != 1) continue;
        const TokenKind before = i > 0 ? tokens_[i - 1].kind : TokenKind::EndOfFile;
        const TokenKind after = i + 1 < tokens_.size() ? tokens_[i + 1].kind : TokenKind::EndOfFile;
        if (before == TokenKind::Dot || before == TokenKind::QuestionDot) continue;
        if (after == TokenKind::KwIn) continue;
        const std::string name(tokens_[i].text);
        bool seen = false;
        for (const auto& d : scope.declared) seen = seen || d == name;
        if (!seen) scope.declared.push_back(name);
    }
}

// 15.7.1: the private names of one class body must be distinct, with exactly
// one exception — a getter and a setter of the same name, both static or both
// not. Everything else is a duplicate, including two getters.
bool Parser::declarePrivateName(const std::string& name, ast::AccessorKind accessor,
                                bool isStatic, Span span) {
    // ClassElementName : PrivateIdentifier — "#constructor" is a Syntax Error
    // wherever it is written, which is not a duplicate rule and not covered by
    // one: there is no `constructor` private element to collide with.
    if (name == "#constructor") {
        diags_.error(span, "'#constructor' is not a valid private name (ECMA-262 15.7.1)");
        return false;
    }
    PrivateNameScope& scope = privateScopes_.back();
    PrivateNameScope::Binding* entry = nullptr;
    for (auto& b : scope.bindings) {
        if (b.name == name) entry = &b;
    }
    if (!entry) {
        scope.bindings.push_back(PrivateNameScope::Binding{name, 0, 0, 0, isStatic, span});
        entry = &scope.bindings.back();
    }
    if (accessor == ast::AccessorKind::Getter) {
        ++entry->getters;
    } else if (accessor == ast::AccessorKind::Setter) {
        ++entry->setters;
    } else {
        ++entry->others;
    }
    const bool accessorPair = entry->others == 0 && entry->getters <= 1 && entry->setters <= 1 &&
                              entry->isStatic == isStatic;
    const bool single = entry->others == 1 && entry->getters == 0 && entry->setters == 0;
    if (!accessorPair && !single) {
        diags_.error(span, "duplicate private name '" + name +
                               "' in a class body (ECMA-262 15.7.1 admits one getter and one "
                               "setter of a name, and nothing else twice)");
        return false;
    }
    return true;
}

bool Parser::privateNameInScope(const std::string& name) const {
    for (size_t i = privateScopes_.size(); i-- > 0;) {
        for (const auto& d : privateScopes_[i].declared) {
            if (d == name) return true;
        }
    }
    return false;
}

// The reference half of 15.7.1, shared by the two forms that can spell one.
// Reported here rather than deferred to lowering because it is a SYNTAX error:
// a program that mentions an undeclared private name has no meaning at all,
// and the position that names it is this one.
static const char* kNoScopeHint =
    "a private name may only be used inside the body of a class that declares it";

bool Parser::parsePrivateMemberLink(ExprPtr& expr, bool optional) {
    const Token& nameTok = advance();  // `#x`
    const std::string name(nameTok.text);
    if (privateScopes_.empty()) {
        diags_.error(nameTok.span, "private name '" + name + "' outside a class body: " +
                                       std::string(kNoScopeHint));
        return false;
    }
    if (!privateNameInScope(name)) {
        diags_.error(nameTok.span,
                     "private name '" + name + "' is not declared by any enclosing class");
        return false;
    }
    auto mem = std::make_unique<MemberAccess>();
    mem->span = {expr->span.begin, nameTok.span.end};
    mem->object = std::move(expr);
    mem->property = name;
    mem->optional = optional;
    mem->isPrivate = true;
    expr = std::move(mem);
    return true;
}

// `#x in o` (13.10.1 RelationalExpression). The private name is not an
// expression on its own — there is nothing it could evaluate to — so this is
// only reached where the `in` that follows it makes it legal, and a `#x`
// anywhere else is refused here by name rather than parsed into a tree no
// later pass could read.
ExprPtr Parser::parsePrivateNameOperand() {
    const Token& nameTok = peek();
    const std::string name(nameTok.text);
    if (peek(1).kind != TokenKind::KwIn) {
        diags_.error(nameTok.span,
                     "private name '" + name +
                         "' may only appear after '.' or on the left of 'in' (ECMA-262 13.10.1)");
        return nullptr;
    }
    if (privateScopes_.empty()) {
        diags_.error(nameTok.span, "private name '" + name + "' outside a class body: " +
                                       std::string(kNoScopeHint));
        return nullptr;
    }
    if (!privateNameInScope(name)) {
        diags_.error(nameTok.span,
                     "private name '" + name + "' is not declared by any enclosing class");
        return nullptr;
    }
    advance();
    auto ident = std::make_unique<Ident>();
    ident->span = nameTok.span;
    ident->name = name;
    return ident;
}

static std::string superExpressionName(const Expr* expr) {
    if (!expr) return "";
    if (const auto* id = dynamic_cast<const Ident*>(expr)) return id->name;
    if (const auto* mem = dynamic_cast<const MemberAccess*>(expr)) {
        std::string prefix = superExpressionName(mem->object.get());
        return prefix.empty() ? mem->property : prefix + "." + mem->property;
    }
    return "";
}

// `class Name [extends Base] { members }`. A class introduces no runtime
// concept - it is the constructor function plus its prototype, and lowering
// desugars it into exactly that. What the parser owes is the shape: which
// member is the constructor, which are static, and which class each `super` in
// a body belongs to.
//
// Everything ES2015+ puts in a class body that bronze has not built is
// diagnosed by name here rather than mis-parsed as a method.
bool Parser::parseClassBodyCommon(const std::string& name, const ast::Expr* superClass,
                                  const std::string& superName,
                                  std::vector<ast::ClassMethod>& methods, Span span) {
    // Before the brace is consumed: the scan indexes tokens, and the body's
    // own `{` is where it starts.
    // The scope is popped however the body is left. Every failure path below
    // is an early return, and a scope left on the stack would make the NEXT
    // class body resolve names that went out of scope with the diagnosed one.
    struct ScopeGuard {
        std::vector<PrivateNameScope>& stack;
        ~ScopeGuard() { stack.pop_back(); }
    };
    privateScopes_.push_back(PrivateNameScope{});
    ScopeGuard scopeGuard{privateScopes_};
    scanPrivateDeclarations(pos_, privateScopes_.back());

    if (!expect(TokenKind::LBrace, "'{' to open a class body")) return false;

    // Every `super` inside a method belongs to THIS class, and the parser is
    // the only place that knows which class that is.
    const std::string savedSuper = currentClassSuper_;
    const ast::Expr* savedSuperExpr = currentClassSuperExpr_;
    const bool savedInMethod = inClassMethod_;
    currentClassSuper_ = superName;
    currentClassSuperExpr_ = superClass;
    inClassMethod_ = true;

    // A ClassElementName is a PropertyName OR a PrivateIdentifier (15.7), and
    // the four places a member name is read all need both spellings.
    auto elementName = [&](const char* what) -> const Token* {
        if (check(TokenKind::PrivateName)) return &advance();
        return expectPropertyName(what);
    };

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
        // follows it, and is an ordinary method/field name in `static() {}` or `static = 1`.
        if (check(TokenKind::Identifier) && peek().text == "static" &&
            peek(1).kind != TokenKind::LParen && peek(1).kind != TokenKind::Assign &&
            peek(1).kind != TokenKind::Semicolon && peek(1).kind != TokenKind::RBrace) {
            advance();
            member.isStatic = true;
        }
        // `static { ... }` — a ClassStaticBlockDefinition (15.7). Its body is
        // held as a function because that is what it is: one body, evaluated
        // once, with the constructor as its `this`. It takes no parameters and
        // nothing may `return` from it, both of which the grammar states by
        // giving it no parameter list at all.
        if (member.isStatic && check(TokenKind::LBrace)) {
            auto fn = std::make_unique<FunctionExpr>();
            fn->span.begin = peek().span.begin;
            // A static block is never handed to a program as a function
            // object, but it is a MethodDefinition's body in every way that
            // matters here: nothing may construct it and it has no prototype.
            fn->kind = ast::FunctionKind::Method;
            fn->name = (name.empty() ? std::string("static") : name + ".static") + ".block" +
                       std::to_string(methods.size());
            fn->strict = true;  // class code, which 15.7 makes strict
            // The `super` of a static block is the class's, exactly as a
            // static method's is: the block is class code, not a nested
            // function written inside it.
            fn->body = parseBlock();
            if (diags_.hasErrors()) {
                ok = false;
                break;
            }
            fn->span.end = previous().span.end;
            member.isStaticBlock = true;
            member.fn = std::move(fn);
            methods.push_back(std::move(member));
            continue;
        }
        // `*m() {}` / `*[expr]() {}` — a generator method
        if (check(TokenKind::Star)) {
            const Token& star = advance();
            if (check(TokenKind::LBracket)) {
                advance();  // '['
                member.keyExpr = parseAssign();
                if (!member.keyExpr ||
                    !expect(TokenKind::RBracket, "']' after computed generator name")) {
                    ok = false;
                    break;
                }
                member.name.clear();
            } else {
                const Token* genName = elementName("generator method name");
                if (!genName) {
                    ok = false;
                    break;
                }
                member.name = std::string(genName->text);
                if (member.isPrivate() &&
                    !declarePrivateName(member.name, ast::AccessorKind::None, member.isStatic,
                                        genName->span)) {
                    ok = false;
                    break;
                }
            }
            auto fn = std::make_unique<FunctionExpr>();
            fn->span.begin = star.span.begin;
            fn->kind = ast::FunctionKind::Method;
            const std::string sym = member.computed()
                                        ? (member.isStatic ? "static.computed" : "computed")
                                        : member.name;
            fn->name = name.empty() ? sym : (name + "." + sym);
            if (!parseGeneratorTail(*fn)) {
                ok = false;
                break;
            }
            member.fn = std::move(fn);
            methods.push_back(std::move(member));
            continue;
        }
        if (check(TokenKind::LBracket)) {
            const Span keySpan = peek().span;
            advance();  // '['
            member.keyExpr = parseAssign();
            if (!member.keyExpr ||
                !expect(TokenKind::RBracket, "']' after computed class member name")) {
                ok = false;
                break;
            }
            member.name.clear();
            if (check(TokenKind::LParen)) {
                auto fn = std::make_unique<FunctionExpr>();
                fn->span.begin = keySpan.begin;
                fn->kind = ast::FunctionKind::Method;
                const std::string sym = member.isStatic ? "static.computed" : "computed";
                fn->name = name.empty() ? sym : (name + "." + sym);
                advance();  // '('
                if (!parseParams(fn->params)) return false;
                if (!expect(TokenKind::RParen, "')' after parameters")) return false;
                if (match(TokenKind::Colon)) fn->returnType = parseTypeAnnotation();
                {
                    GeneratorScopeGuard guard(*this);
                    fn->body = parseFunctionBody(fn->strict);
                }
                if (!checkStrictParams(fn->params, fn->strict)) return false;
                if (diags_.hasErrors()) return false;
                fn->span.end = previous().span.end;
                member.fn = std::move(fn);
                methods.push_back(std::move(member));
                continue;
            }
            // Computed field: `[expr] = val;` or `[expr];`
            member.isField = true;
            if (match(TokenKind::Assign)) {
                member.init = parseAssign();
                if (!member.init) {
                    ok = false;
                    break;
                }
            }
            match(TokenKind::Semicolon);
            methods.push_back(std::move(member));
            continue;
        }
        // `async` is contextual as well, and a ClassElementName on the SAME
        // line is what makes it a modifier (ECMA-262 15.8.1 forbids a line
        // terminator after it) — `async() {}` is a method named `async` and
        // `async = 1` a field named `async`. Handled here, ahead of the field
        // diagnostic below, which fires on the identifier-then-identifier
        // shape and would call an async method a field.
        if (check(TokenKind::Identifier) && peek().text == "async" && !peek(1).newlineBefore &&
            (isIdentifierName(peek(1).kind) || peek(1).kind == TokenKind::Star ||
             peek(1).kind == TokenKind::LBracket || peek(1).kind == TokenKind::PrivateName ||
             peek(1).kind == TokenKind::StringLiteral ||
             peek(1).kind == TokenKind::NumberLiteral)) {
            // 15.8's MethodDefinition begins at `async`, so that is where the
            // member's source text begins — not at the name or the `*`.
            const Span asyncKwSpan = advance().span;  // `async`
            if (check(TokenKind::Star)) {
                advance();  // `*`
                if (check(TokenKind::LBracket)) {
                    advance();  // '['
                    member.keyExpr = parseAssign();
                    if (!member.keyExpr ||
                        !expect(TokenKind::RBracket, "']' after computed async generator name")) {
                        ok = false;
                        break;
                    }
                    member.name.clear();
                } else {
                    const Token* genName = elementName("async generator method name");
                    if (!genName) {
                        ok = false;
                        break;
                    }
                    member.name = std::string(genName->text);
                    if (member.isPrivate() &&
                        !declarePrivateName(member.name, ast::AccessorKind::None, member.isStatic,
                                            genName->span)) {
                        ok = false;
                        break;
                    }
                }
                const std::string sym = member.computed()
                                            ? (member.isStatic ? "static.computed" : "computed")
                                            : member.name;
                const std::string fnName = name.empty() ? sym : (name + "." + sym);
                auto fn = parseAsyncMethodTail(fnName, asyncKwSpan, /*clearSuper=*/false,
                                               /*isGenerator=*/true);
                if (!fn) {
                    ok = false;
                    break;
                }
                member.fn = std::move(fn);
                methods.push_back(std::move(member));
                continue;
            }
            Span asyncSpan = asyncKwSpan;
            if (check(TokenKind::LBracket)) {
                advance();  // '['
                member.keyExpr = parseAssign();
                if (!member.keyExpr ||
                    !expect(TokenKind::RBracket, "']' after computed async method name")) {
                    ok = false;
                    break;
                }
                member.name.clear();
            } else {
                const Token* asyncName = elementName("async method name");
                if (!asyncName) {
                    ok = false;
                    break;
                }
                member.name = std::string(asyncName->text);
                if (member.isPrivate() &&
                    !declarePrivateName(member.name, ast::AccessorKind::None, member.isStatic,
                                        asyncName->span)) {
                    ok = false;
                    break;
                }
            }
            const std::string sym = member.computed()
                                        ? (member.isStatic ? "static.computed" : "computed")
                                        : member.name;
            const std::string fnName = name.empty() ? sym : (name + "." + sym);
            auto fn = parseAsyncMethodTail(fnName, asyncSpan,
                                           /*clearSuper=*/false);
            if (!fn) {
                ok = false;
                break;
            }
            member.fn = std::move(fn);
            methods.push_back(std::move(member));
            continue;
        }
        // `get`/`set` are contextual here too: `get() {}` is a method named
        // `get`, and only a following name makes this an accessor.
        if (check(TokenKind::Identifier) && (peek().text == "get" || peek().text == "set") &&
            (peek(1).kind == TokenKind::Identifier || peek(1).kind == TokenKind::StringLiteral ||
             peek(1).kind == TokenKind::PrivateName || peek(1).kind == TokenKind::LBracket)) {
            const ast::AccessorKind kind =
                peek().text == "get" ? ast::AccessorKind::Getter : ast::AccessorKind::Setter;
            advance();  // 'get' / 'set'
            const Span accessorNameSpan = peek().span;
            auto accessorFn = parseAccessorMember(kind, member.name, &member.keyExpr);
            if (!accessorFn) {
                ok = false;
                break;
            }
            if (member.isPrivate() &&
                !declarePrivateName(member.name, kind, member.isStatic, accessorNameSpan)) {
                ok = false;
                break;
            }
            accessorFn->name = name.empty() ? accessorFn->name : (name + "." + accessorFn->name);
            member.accessor = kind;
            member.fn = std::move(accessorFn);
            methods.push_back(std::move(member));
            continue;
        }
        // A ClassElementName is a PropertyName is an IdentifierName (15.7), so
        // `delete()` and `return()` are ordinary method names on a class.
        const Token* memberName = elementName("class member name");
        if (!memberName) {
            ok = false;
            break;
        }
        member.name = std::string(memberName->text);
        if (member.isPrivate() &&
            !declarePrivateName(member.name, ast::AccessorKind::None, member.isStatic,
                                memberName->span)) {
            ok = false;
            break;
        }
        if (!check(TokenKind::LParen)) {
            // Field: `name = val;` or `name;`
            member.isField = true;
            if (match(TokenKind::Assign)) {
                member.init = parseAssign();
                if (!member.init) {
                    ok = false;
                    break;
                }
            }
            match(TokenKind::Semicolon);
            methods.push_back(std::move(member));
            continue;
        }
        member.isConstructor = !member.isStatic && member.name == "constructor";

        auto fn = std::make_unique<FunctionExpr>();
        fn->span.begin = memberName->span.begin;
        // 15.7.14 step 11: the `constructor` element becomes the class object
        // itself, which IS a constructor and has a `prototype`; every other
        // element is a MethodDefinition and has neither.
        fn->kind = member.isConstructor ? ast::FunctionKind::ClassConstructor
                                        : ast::FunctionKind::Method;
        fn->name = name.empty() ? member.name : (name + "." + member.name);
        advance();  // '('
        if (!parseParams(fn->params)) return false;
        if (!expect(TokenKind::RParen, "')' after parameters")) return false;
        if (match(TokenKind::Colon)) fn->returnType = parseTypeAnnotation();
        {
            GeneratorScopeGuard guard(*this);
            fn->body = parseFunctionBody(fn->strict);
        }
        if (!checkStrictParams(fn->params, fn->strict)) return false;
        if (diags_.hasErrors()) return false;
        fn->span.end = previous().span.end;
        member.fn = std::move(fn);
        methods.push_back(std::move(member));
    }

    currentClassSuper_ = savedSuper;
    currentClassSuperExpr_ = savedSuperExpr;
    inClassMethod_ = savedInMethod;
    if (!ok) return false;
    if (!expect(TokenKind::RBrace, "'}' to close a class body")) return false;

    // Lowering wants exactly one constructor, always, so a class that writes
    // none gets the one ECMA-262 15.7.14 says it has — synthesized here, in
    // the parser, so lowering never has two shapes to reason about.
    //
    // For a base class that is an empty body. For a DERIVED one it is
    // `constructor(...args) { super(...args); }`, which is exactly a rest
    // parameter and a spread and nothing else: the forwarding has to be
    // arity-preserving, and before those existed it was a named error rather
    // than a quietly truncated argument list.
    bool hasCtor = false;
    for (const auto& m : methods) hasCtor = hasCtor || m.isConstructor;
    if (!hasCtor) {
        ClassMethod ctor;
        ctor.name = "constructor";
        ctor.isConstructor = true;
        ctor.fn = std::make_unique<FunctionExpr>();
        ctor.fn->kind = ast::FunctionKind::ClassConstructor;
        ctor.fn->name = name.empty() ? "constructor" : (name + ".constructor");
        ctor.fn->span = span;
        ctor.fn->strict = true;  // synthesized class code, which 15.7 makes strict
        if (superClass || !superName.empty()) {
            Param rest;
            rest.name = "args";
            rest.isRest = true;
            rest.span = span;
            ctor.fn->params.push_back(std::move(rest));

            auto argsRef = std::make_unique<Ident>();
            argsRef->span = span;
            argsRef->name = "args";
            auto spread = std::make_unique<SpreadElement>();
            spread->span = span;
            spread->argument = std::move(argsRef);

            auto call = std::make_unique<SuperCall>();
            call->span = span;
            call->baseName = superName;
            if (superClass) call->baseExpr = ast::cloneExpr(*superClass);
            call->args.push_back(std::move(spread));

            auto stmt = std::make_unique<ExprStmt>();
            stmt->span = span;
            stmt->expr = std::move(call);
            ctor.fn->body.push_back(std::move(stmt));
        }
        methods.insert(methods.begin(), std::move(ctor));
    }
    return true;
}

ast::StmtPtr Parser::parseClass(const std::string& defaultName) {
    const Token& kw = advance();  // 'class'
    // ECMA-262 10.2.11 / 15.7: ALL parts of a class definition are strict mode
    // code, whether or not anything said so — the name it binds, the `extends`
    // clause, and every method body. Raised once here rather than per member,
    // so that "a class body is strict" is one statement in the code and not
    // one for each of the six ways a member can be written.
    StrictScopeGuard strictGuard(*this);
    strict_ = true;
    auto cls = std::make_unique<ClassDecl>();
    cls->span.begin = kw.span.begin;
    if (!defaultName.empty() && !(check(TokenKind::Identifier) || check(TokenKind::KwOf))) {
        cls->name = defaultName;  // `export default class {}`, as above
    } else {
        const Token* nameTok = expect(TokenKind::Identifier, "class name");
        if (!nameTok) return nullptr;
        if (!checkStrictBindingName(nameTok->text, nameTok->span, "class name")) return nullptr;
        cls->name = std::string(nameTok->text);
    }

    if (match(TokenKind::KwExtends)) {
        cls->superClass = parseUnaryPostfix();
        if (!cls->superClass) return nullptr;
        cls->superName = superExpressionName(cls->superClass.get());
    }

    if (!parseClassBodyCommon(cls->name, cls->superClass.get(), cls->superName, cls->methods, cls->span)) return nullptr;
    cls->span.end = previous().span.end;
    return cls;
}

ast::ExprPtr Parser::parseClassExpr() {
    const Token& kw = advance();  // 'class'
    StrictScopeGuard strictGuard(*this);
    strict_ = true;
    auto cls = std::make_unique<ClassExpr>();
    cls->span.begin = kw.span.begin;
    if (check(TokenKind::Identifier) || check(TokenKind::KwOf)) {
        const Token& nameTok = advance();
        if (!checkStrictBindingName(nameTok.text, nameTok.span, "class name")) return nullptr;
        cls->name = std::string(nameTok.text);
    }

    if (match(TokenKind::KwExtends)) {
        cls->superClass = parseUnaryPostfix();
        if (!cls->superClass) return nullptr;
        cls->superName = superExpressionName(cls->superClass.get());
    }

    if (!parseClassBodyCommon(cls->name, cls->superClass.get(), cls->superName, cls->methods, cls->span)) return nullptr;
    cls->span.end = previous().span.end;
    return cls;
}

}  // namespace bronze
