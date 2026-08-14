// Every form that has a parameter list and a body: function declarations and
// expressions, arrows, and classes — which are the same thing again, with the
// bookkeeping that tells each `super` which class it belongs to.

#include "parse/parser.h"

namespace bronze {

using namespace ast;

namespace {

// The IL symbol a class's `[Symbol.iterator]` method compiles to. A computed
// key has no NAME — the two fields of a class member are never both meaningful
// — but a function still needs a symbol, and this one cannot collide with a
// method a program wrote: a source identifier cannot contain a dot.
constexpr const char* kIteratorMethodSymbol = "Symbol.iterator";

}  // namespace

StmtPtr Parser::parseFunctionDecl(bool isExported, const std::string& defaultName) {
    const Token& kw = advance();  // 'function'
    auto fn = std::make_unique<FunctionDecl>();
    fn->span.begin = kw.span.begin;
    fn->isExported = isExported;
    const bool isGenerator = match(TokenKind::Star);

    if (!defaultName.empty() && !check(TokenKind::Identifier)) {
        // `export default function () {}` (ECMA-262 16.2.3.7): a hoisted
        // declaration with no name. It is still hoisted and still a
        // declaration, so it gets the reserved word `default` as its name —
        // which nothing in the source can spell, so it cannot collide.
        fn->name = defaultName;
    } else {
        const Token* name = expect(TokenKind::Identifier, "function name");
        if (!name) return nullptr;
        if (!checkStrictBindingName(name->text, name->span, "function name")) return nullptr;
        fn->name = std::string(name->text);
    }

    // `function* g() {}` takes the same route a generator METHOD does: a
    // FunctionExpr is filled in and its pieces moved across, because a
    // declaration and an expression differ in where the value lands and in
    // nothing about the body.
    if (isGenerator) {
        GeneratorScopeGuard guard(*this);
        ast::FunctionExpr shell;
        shell.span = fn->span;
        shell.name = fn->name;
        if (!parseGeneratorTail(shell)) return nullptr;
        fn->params = std::move(shell.params);
        fn->returnType = std::move(shell.returnType);
        fn->body = std::move(shell.body);
        fn->strict = shell.strict;
        fn->isGenerator = true;
        fn->span.end = peek().span.begin;
        return fn;
    }

    if (!expect(TokenKind::LParen, "'(' after function name")) return nullptr;
    if (!parseParams(fn->params)) return nullptr;
    if (!expect(TokenKind::RParen, "')' after parameters")) return nullptr;
    if (match(TokenKind::Colon)) fn->returnType = parseTypeAnnotation();

    {
        GeneratorScopeGuard guard(*this);
        fn->body = parseFunctionBody(fn->strict);
    }
    if (!checkStrictParams(fn->params, fn->strict)) return nullptr;
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

    // An arrow written inside a generator body is not itself a generator, so
    // `yield` is an ordinary identifier in it and `return` an ordinary return.
    GeneratorScopeGuard guard(*this);
    if (check(TokenKind::LBrace)) {
        fn->body = parseFunctionBody(fn->strict);
    } else {
        // A concise body has no Directive Prologue to read — there is no
        // StatementList for one to be the head of — so an arrow written this
        // way is strict exactly when the code around it is.
        fn->strict = strict_;
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
    if (!checkStrictParams(fn->params, fn->strict)) return nullptr;
    if (diags_.hasErrors()) return nullptr;
    fn->span.end = peek().span.begin;
    return fn;
}
// One parameter list, for every function form there is — declaration,
// expression, arrow and class method. One copy, because four copies of this
// loop are four places for the rules below to drift apart.
//
// A parameter is a binding target (a name or a pattern), optionally with a
// default; or a rest parameter, which takes what is left and so must be last
// and can have neither a default nor a pattern.
bool Parser::parseParams(std::vector<ast::Param>& out) {
    while (!check(TokenKind::RParen)) {
        if (check(TokenKind::EndOfFile)) {
            error("unterminated parameter list");
            return false;
        }
        Param p;
        p.span.begin = peek().span.begin;
        const bool isRest = match(TokenKind::Ellipsis);
        p.isRest = isRest;

        if (check(TokenKind::LBracket) || check(TokenKind::LBrace)) {
            if (isRest) {
                error("a rest parameter must be a plain name");
                return false;
            }
            p.pattern = parsePattern();
            if (!p.pattern) return false;
        } else {
            const Token* param = expect(TokenKind::Identifier, "parameter name");
            if (!param) return false;
            // What fires here is the ENCLOSING code's mode. A list belonging to
            // a function whose own directive has not been read yet is checked
            // again by `checkStrictParams`, once the body has said so.
            if (!checkStrictBindingName(param->text, param->span, "parameter")) return false;
            p.name = std::string(param->text);
        }

        if (match(TokenKind::Colon)) p.typeAnnotation = parseTypeAnnotation();
        if (check(TokenKind::Assign)) {
            if (isRest) {
                error("a rest parameter may not have a default value");
                return false;
            }
            advance();
            p.defaultValue = parseAssign();
            if (!p.defaultValue) return false;
        }
        p.span.end = peek().span.begin;
        out.push_back(std::move(p));

        if (isRest) {
            if (!check(TokenKind::RParen)) {
                error("a rest parameter must be the last parameter");
                return false;
            }
            break;
        }
        if (!match(TokenKind::Comma)) break;
    }
    return true;
}

// `get k() {}` / `set k(v) {}`, with the contextual `get` or `set` already
// consumed. Object literals and class bodies share this because they share
// the whole of the syntax: what differs is the enumerable attribute the
// runtime gives the result, which is the caller's to decide.
//
// The arity rules are 15.4.1's and are early errors, not runtime ones: a
// getter that took a parameter could never be given one, and a setter that
// took none would silently discard every write.
std::unique_ptr<ast::FunctionExpr> Parser::parseAccessorMember(ast::AccessorKind kind,
                                                               std::string& outName) {
    const bool isGetter = kind == ast::AccessorKind::Getter;
    const char* word = isGetter ? "getter" : "setter";

    if (check(TokenKind::LBracket)) {
        error((std::string("unsupported construct: a computed ") + word +
               " name (`get [e]() {}`)")
                  .c_str());
        return nullptr;
    }
    Span nameSpan = peek().span;
    if (check(TokenKind::StringLiteral)) {
        const Token& sTok = advance();
        outName = decodeStringLiteral(sTok.text.substr(1, sTok.text.size() - 2), sTok.span);
    } else if (isIdentifierName(peek().kind)) {
        outName = std::string(advance().text);
    } else {
        // A numeric accessor name lands here with the object literal's own
        // rule: the name is ToString(Number), which is the runtime's
        // formatter and not something the parser may reimplement.
        error((std::string("expected a ") + word +
               " name: an identifier or a string literal")
                  .c_str());
        return nullptr;
    }

    auto fn = std::make_unique<FunctionExpr>();
    fn->span.begin = nameSpan.begin;
    fn->name = std::string(isGetter ? "get " : "set ") + outName;
    if (!expect(TokenKind::LParen, "'(' after an accessor name")) return nullptr;
    if (!parseParams(fn->params)) return nullptr;
    if (!expect(TokenKind::RParen, "')' after accessor parameters")) return nullptr;
    if (match(TokenKind::Colon)) fn->returnType = parseTypeAnnotation();

    const size_t want = isGetter ? 0u : 1u;
    if (fn->params.size() != want) {
        error((std::string("a ") + word + " must take exactly " +
               (isGetter ? "no parameters" : "one parameter"))
                  .c_str());
        return nullptr;
    }
    if (!fn->params.empty() && fn->params[0].isRest) {
        error("a setter's parameter may not be a rest parameter");
        return nullptr;
    }

    {
        GeneratorScopeGuard guard(*this);
        fn->body = parseFunctionBody(fn->strict);
    }
    if (!checkStrictParams(fn->params, fn->strict)) return nullptr;
    if (diags_.hasErrors()) return nullptr;
    fn->span.end = peek().span.begin;
    return fn;
}

// `m(params) { body }` with the name already consumed — the tail of every
// MethodDefinition (ECMA-262 15.4), which an object literal and a class body
// spell identically.
//
// A method's `super` is resolved against its HOME OBJECT, and an object
// literal's home object is the literal. bronze resolves `super` by the class
// name the parser is inside, so an object literal method nested in a class
// method must not inherit that binding: `super.m()` there would silently call
// the enclosing class's parent. The two fields are therefore saved and cleared
// for the body, which turns that into the existing "super outside a class
// method" error.
std::unique_ptr<ast::FunctionExpr> Parser::parseMethodTail(const std::string& name,
                                                           Span nameSpan) {
    auto fn = std::make_unique<FunctionExpr>();
    fn->span.begin = nameSpan.begin;
    fn->name = name;
    if (!expect(TokenKind::LParen, "'(' after a method name")) return nullptr;
    if (!parseParams(fn->params)) return nullptr;
    if (!expect(TokenKind::RParen, "')' after method parameters")) return nullptr;
    if (match(TokenKind::Colon)) fn->returnType = parseTypeAnnotation();

    const bool savedInClassMethod = inClassMethod_;
    const std::string savedClassSuper = currentClassSuper_;
    inClassMethod_ = false;
    currentClassSuper_.clear();
    GeneratorScopeGuard guard(*this);
    fn->body = parseFunctionBody(fn->strict);
    inClassMethod_ = savedInClassMethod;
    currentClassSuper_ = savedClassSuper;

    if (!checkStrictParams(fn->params, fn->strict)) return nullptr;
    if (diags_.hasErrors()) return nullptr;
    fn->span.end = peek().span.begin;
    return fn;
}

// `class Name [extends Base] { members }`. A class introduces no runtime
// concept - it is the constructor function plus its prototype, and lowering
// desugars it into exactly that. What the parser owes is the shape: which
// member is the constructor, which are static, and which class each `super` in
// a body belongs to.
//
// Everything ES2015+ puts in a class body that bronze has not built -
// fields, getters and setters, computed keys, generators - is diagnosed by
// name here rather than mis-parsed as a method.
bool Parser::parseClassBodyCommon(const std::string& name, const std::string& superName,
                                  std::vector<ast::ClassMethod>& methods, Span span) {
    if (!expect(TokenKind::LBrace, "'{' to open a class body")) return false;

    // Every `super` inside a method belongs to THIS class, and the parser is
    // the only place that knows which class that is.
    const std::string savedSuper = currentClassSuper_;
    const bool savedInMethod = inClassMethod_;
    currentClassSuper_ = superName;
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
        // `*m() {}` / `*[Symbol.iterator]() {}` — a generator method, which the
        // parser desugars into an ordinary method returning an iterator object.
        // The name is taken here because a generator's is spelled the two ways
        // a method's is, and `[Symbol.iterator]` is the only computed one
        // bronze reads.
        if (check(TokenKind::Star)) {
            const Token& star = advance();
            member.keyExpr = matchSymbolIteratorKey();
            if (member.keyExpr) {
                // `name` and `keyExpr` are never both meaningful, the same rule
                // an object literal's property follows: a computed key has no
                // name until it is evaluated. The FUNCTION still needs an IL
                // symbol, and that is what `kIteratorMethodSymbol` spells.
                member.name.clear();
            } else {
                if (check(TokenKind::LBracket)) {
                    error("unsupported construct: a computed generator name in a class body "
                          "(only `*[Symbol.iterator]()` is read)");
                    ok = false;
                    break;
                }
                const Token* genName = expectPropertyName("generator method name");
                if (!genName) {
                    ok = false;
                    break;
                }
                member.name = std::string(genName->text);
            }
            auto fn = std::make_unique<FunctionExpr>();
            fn->span.begin = star.span.begin;
            fn->name = name.empty()
                           ? (member.keyExpr ? std::string(kIteratorMethodSymbol) : member.name)
                           : (name + "." + (member.keyExpr ? std::string(kIteratorMethodSymbol)
                                                           : member.name));
            if (!parseGeneratorTail(*fn)) {
                ok = false;
                break;
            }
            member.fn = std::move(fn);
            methods.push_back(std::move(member));
            continue;
        }
        if (check(TokenKind::LBracket)) {
            // The same one computed key, without the `*`: an iterator written
            // out by hand rather than as a generator. One rule for what a
            // computed class member name may be, not a generator-only one.
            const Span keySpan = peek().span;
            member.keyExpr = matchSymbolIteratorKey();
            if (member.keyExpr) {
                member.name.clear();
                if (!check(TokenKind::LParen)) {
                    error("unsupported construct: a `[Symbol.iterator]` class field "
                          "(only methods are supported)");
                    ok = false;
                    break;
                }
                // Parsed inline rather than through `parseMethodTail`, which
                // clears the enclosing class's `super` binding because an
                // object literal's home object is the literal. This IS a
                // class method and its `super` is the class's.
                auto fn = std::make_unique<FunctionExpr>();
                fn->span.begin = keySpan.begin;
                fn->name = name.empty() ? std::string(kIteratorMethodSymbol)
                                        : (name + "." + kIteratorMethodSymbol);
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
                fn->span.end = peek().span.begin;
                member.fn = std::move(fn);
                methods.push_back(std::move(member));
                continue;
            }
            error("unsupported construct: computed method name in a class body");
            ok = false;
            break;
        }
        // `async` is contextual as well, and a ClassElementName on the SAME
        // line is what makes it a modifier (ECMA-262 15.8.1 forbids a line
        // terminator after it) — `async() {}` is a method named `async` and
        // `async = 1` a field named `async`. Handled here, ahead of the field
        // diagnostic below, which fires on the identifier-then-identifier
        // shape and would call an async method a field.
        if (check(TokenKind::Identifier) && peek().text == "async" && !peek(1).newlineBefore &&
            (isIdentifierName(peek(1).kind) || peek(1).kind == TokenKind::Star ||
             peek(1).kind == TokenKind::LBracket ||
             peek(1).kind == TokenKind::StringLiteral ||
             peek(1).kind == TokenKind::NumberLiteral)) {
            advance();  // `async`
            if (check(TokenKind::Star)) {
                error("unsupported construct: an async generator method in a class body "
                      "(`async *m() {}`)");
                ok = false;
                break;
            }
            if (check(TokenKind::LBracket)) {
                error("unsupported construct: a computed async method name in a class body");
                ok = false;
                break;
            }
            const Token* asyncName = expectPropertyName("async method name");
            if (!asyncName) {
                ok = false;
                break;
            }
            member.name = std::string(asyncName->text);
            const std::string fnName = name.empty() ? member.name : (name + "." + member.name);
            // A class's own method keeps the enclosing `super` binding, which
            // is why the tail is told not to clear it — the same split
            // parseMethodTail and the inline `[Symbol.iterator]` above make.
            auto fn = parseAsyncMethodTail(fnName, asyncName->span,
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
             peek(1).kind == TokenKind::LBracket)) {
            const ast::AccessorKind kind =
                peek().text == "get" ? ast::AccessorKind::Getter : ast::AccessorKind::Setter;
            advance();  // 'get' / 'set'
            auto accessorFn = parseAccessorMember(kind, member.name);
            if (!accessorFn) {
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
        const Token* memberName = expectPropertyName("class member name");
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
        fn->span.end = peek().span.begin;
        member.fn = std::move(fn);
        methods.push_back(std::move(member));
    }

    currentClassSuper_ = savedSuper;
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
        ctor.fn->name = name.empty() ? "constructor" : (name + ".constructor");
        ctor.fn->span = span;
        ctor.fn->strict = true;  // synthesized class code, which 15.7 makes strict
        if (!superName.empty()) {
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
    if (!defaultName.empty() && !check(TokenKind::Identifier)) {
        cls->name = defaultName;  // `export default class {}`, as above
    } else {
        const Token* nameTok = expect(TokenKind::Identifier, "class name");
        if (!nameTok) return nullptr;
        if (!checkStrictBindingName(nameTok->text, nameTok->span, "class name")) return nullptr;
        cls->name = std::string(nameTok->text);
    }

    if (match(TokenKind::KwExtends)) {
        const Token* base = expect(TokenKind::Identifier, "base class name after 'extends'");
        if (!base) return nullptr;
        cls->superName = std::string(base->text);
    }

    if (!parseClassBodyCommon(cls->name, cls->superName, cls->methods, cls->span)) return nullptr;
    cls->span.end = peek().span.begin;
    return cls;
}

ast::ExprPtr Parser::parseClassExpr() {
    const Token& kw = advance();  // 'class'
    StrictScopeGuard strictGuard(*this);
    strict_ = true;
    auto cls = std::make_unique<ClassExpr>();
    cls->span.begin = kw.span.begin;
    if (check(TokenKind::Identifier)) {
        const Token& nameTok = advance();
        if (!checkStrictBindingName(nameTok.text, nameTok.span, "class name")) return nullptr;
        cls->name = std::string(nameTok.text);
    }

    if (match(TokenKind::KwExtends)) {
        const Token* base = expect(TokenKind::Identifier, "base class name after 'extends'");
        if (!base) return nullptr;
        cls->superName = std::string(base->text);
    }

    if (!parseClassBodyCommon(cls->name, cls->superName, cls->methods, cls->span)) return nullptr;
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
        const Token* member = expectPropertyName("property name after 'super.'");
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
    const bool isGenerator = match(TokenKind::Star);

    if (check(TokenKind::Identifier)) {
        const Token& nameTok = advance();
        if (!checkStrictBindingName(nameTok.text, nameTok.span, "function name")) return nullptr;
        fn->name = std::string(nameTok.text);
    }

    GeneratorScopeGuard guard(*this);
    if (isGenerator) {
        if (!parseGeneratorTail(*fn)) return nullptr;
        return fn;
    }

    if (!expect(TokenKind::LParen, "'(' after function")) return nullptr;
    if (!parseParams(fn->params)) return nullptr;
    if (!expect(TokenKind::RParen, "')' after parameters")) return nullptr;
    if (match(TokenKind::Colon)) fn->returnType = parseTypeAnnotation();

    fn->body = parseFunctionBody(fn->strict);
    if (!checkStrictParams(fn->params, fn->strict)) return nullptr;
    if (diags_.hasErrors()) return nullptr;
    fn->span.end = peek().span.begin;
    return fn;
}

}  // namespace bronze
