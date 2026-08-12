// Generators, and the desugaring that is the whole of bronze's support for
// them (docs/0026). A generator whose body is a straight-line sequence of
// `yield <expr>;` statements is rewritten HERE, in the parser, into an
// iterator object over a step index — so no node for `yield` reaches the AST,
// and inference, lowering and the backend never learn that generators exist.
//
//     *[Symbol.iterator]() { yield this.x; yield this.y; }
//
// becomes
//
//     @@iterator() {
//         let step = 0;
//         return {
//             next: () => {
//                 if (step === 0) { step = 1; return { value: this.x, done: false }; }
//                 if (step === 1) { step = 2; return { value: this.y, done: false }; }
//                 return { value: undefined, done: true };
//             },
//             "@@iterator"() { return this; }
//         };
//     }
//
// `next` is an ARROW so that `this` in a yielded expression is the receiver
// the generator method was called on, which is the whole reason the three.js
// generators exist. The step variable lives in the enclosing invocation's
// environment record, so two calls to the method walk independently and a
// second walk starts fresh (docs/0007).
//
// Everything outside that subset is refused BY NAME rather than approximated,
// because the approximation would be a silent wrong answer: an index switch
// re-enters the body from the top on every `next`, so a `yield` inside a loop
// would repeat the loop, and a binding declared between two yields would not
// survive the return in between.

#include <string>
#include <utility>

#include "parse/parser.h"

namespace bronze {

using namespace ast;

namespace {

// The step of a desugared generator: what runs before it, and what it yields.
struct GeneratorStep {
    std::vector<StmtPtr> before;
    ExprPtr value;
    Span span;
};

ExprPtr numberLit(double value, Span span) {
    auto lit = std::make_unique<NumberLit>();
    lit->span = span;
    lit->value = value;
    return lit;
}

ExprPtr identExpr(const std::string& name, Span span) {
    auto ident = std::make_unique<Ident>();
    ident->span = span;
    ident->name = name;
    return ident;
}

ExprPtr binaryExpr(BinaryOp op, ExprPtr lhs, ExprPtr rhs, Span span) {
    auto bin = std::make_unique<Binary>();
    bin->span = span;
    bin->op = op;
    bin->lhs = std::move(lhs);
    bin->rhs = std::move(rhs);
    return bin;
}

StmtPtr exprStmt(ExprPtr expr, Span span) {
    auto stmt = std::make_unique<ExprStmt>();
    stmt->span = span;
    stmt->expr = std::move(expr);
    return stmt;
}

StmtPtr returnStmt(ExprPtr value, Span span) {
    auto stmt = std::make_unique<ReturnStmt>();
    stmt->span = span;
    stmt->value = std::move(value);
    return stmt;
}

// `{ value: <v>, done: <d> }` — the IteratorResult 7.4.1 requires of every
// `next`, built as an ordinary object literal because that is exactly what a
// program would have written.
ExprPtr iterResult(ExprPtr value, bool done, Span span) {
    auto obj = std::make_unique<ObjectLit>();
    obj->span = span;

    ObjectProp valueProp;
    valueProp.key = "value";
    valueProp.value = std::move(value);
    obj->props.push_back(std::move(valueProp));

    auto doneLit = std::make_unique<BoolLit>();
    doneLit->span = span;
    doneLit->value = done;
    ObjectProp doneProp;
    doneProp.key = "done";
    doneProp.value = std::move(doneLit);
    obj->props.push_back(std::move(doneProp));

    return obj;
}

// `step = <n>;`
StmtPtr setStep(const std::string& stepName, double n, Span span) {
    return exprStmt(
        binaryExpr(BinaryOp::Assign, identExpr(stepName, span), numberLit(n, span), span), span);
}

// `if (step === <n>) { <body> }`
StmtPtr stepGuard(const std::string& stepName, double n, std::vector<StmtPtr> body, Span span) {
    auto stmt = std::make_unique<IfStmt>();
    stmt->span = span;
    stmt->condition =
        binaryExpr(BinaryOp::StrictEq, identExpr(stepName, span), numberLit(n, span), span);
    stmt->thenBody = std::move(body);
    return stmt;
}

// What a top-level statement of a generator body would put a `yield` inside
// of. Only the OUTERMOST construct is named, which is the one the message
// should talk about: a `yield` three blocks down inside a `for` is a yield in
// a loop, and saying "a nested block" would send the reader to the wrong
// line. Null means the statement is one whose refusal is about the yield's
// VALUE being used rather than about where it sits.
const char* refusalForStatement(TokenKind kind) {
    switch (kind) {
        case TokenKind::KwFor:
        case TokenKind::KwWhile:
        case TokenKind::KwDo:
            return "a loop";
        case TokenKind::KwIf:
            return "an `if`";
        case TokenKind::KwSwitch:
            return "a `switch`";
        case TokenKind::KwTry:
            return "a `try`";
        case TokenKind::LBrace:
            return "a nested block";
        default:
            return nullptr;
    }
}

// The refusal used for every other position: a declarator's initializer, an
// argument, an operand — all of them places where the value a `yield`
// produces would be read, and bronze's `next` has no way to deliver one
// because the value a generator is resumed WITH arrives on the next call.
constexpr const char* kValueUsedRefusal = "an expression whose value is used";

constexpr const char* kSubsetNote =
    "bronze implements the straight-line subset only: a generator body is a "
    "sequence of `yield <expr>;` statements, desugared into an iterator over a "
    "step index (docs/0026)";

}  // namespace

ExprPtr Parser::refuseYield() {
    const Token& tok = peek();
    const bool delegating = peek(1).kind == TokenKind::Star;
    std::string message;
    if (delegating) {
        message = std::string("unsupported construct: `yield*` (delegation); ") + kSubsetNote;
    } else {
        const char* where = yieldRefusal_ ? yieldRefusal_ : kValueUsedRefusal;
        message = std::string("unsupported construct: a `yield` inside ") + where + "; " +
                  kSubsetNote;
    }
    diags_.error(tok.span, message);
    return nullptr;
}

// `[ Symbol.iterator ]`, the one computed member name bronze reads. It is
// matched SYNTACTICALLY, on the two identifiers, rather than evaluated:
// docs/0021 decision 1 makes `Symbol.iterator` the string `"@@iterator"` at
// compile time, and a class body has no place to run an expression for a key.
// The divergence is that a program which rebinds `Symbol` still gets
// `@@iterator` here; that is the same bet docs/0011 decision 1 makes for every
// provided global, and `Symbol` is on that list.
bool Parser::matchSymbolIteratorKey(std::string& outName) {
    if (!check(TokenKind::LBracket)) return false;
    if (!(peek(1).kind == TokenKind::Identifier && peek(1).text == "Symbol")) return false;
    if (peek(2).kind != TokenKind::Dot) return false;
    if (!(peek(3).kind == TokenKind::Identifier && peek(3).text == "iterator")) return false;
    if (peek(4).kind != TokenKind::RBracket) return false;
    advance();  // '['
    advance();  // 'Symbol'
    advance();  // '.'
    advance();  // 'iterator'
    advance();  // ']'
    outName = "@@iterator";
    return true;
}

bool Parser::parseGeneratorTail(ast::FunctionExpr& fn) {
    if (!expect(TokenKind::LParen, "'(' after a generator name")) return false;
    if (!parseParams(fn.params)) return false;
    if (!expect(TokenKind::RParen, "')' after generator parameters")) return false;
    if (match(TokenKind::Colon)) fn.returnType = parseTypeAnnotation();

    const Span bodySpan = peek().span;
    if (!expect(TokenKind::LBrace, "'{' to open a generator body")) return false;

    const size_t ordinal = generatorOrdinal_++;
    const std::string qualifier =
        fileId_ == 0 ? std::to_string(ordinal) : std::to_string(fileId_) + "." + std::to_string(ordinal);
    // Dots, for the reason an object-literal method's IL symbol has them: a
    // source identifier cannot contain one, so none of these names can be
    // confused with — or shadowed by — a binding the program wrote.
    const std::string stepName = "gen." + qualifier + ".step";

    std::vector<GeneratorStep> steps;
    // Statements seen since the last `yield`. They belong to the step the
    // NEXT yield opens — a generator body does not run until the first
    // `next()`, and each stretch between two yields runs on the call that
    // produces the second — so they are held here until that yield arrives,
    // and what is left over at the end is the tail that runs on the call that
    // reports `done`.
    std::vector<StmtPtr> pending;

    const bool savedInBody = inGeneratorBody_;
    const char* savedRefusal = yieldRefusal_;
    inGeneratorBody_ = true;

    bool ok = true;
    while (!check(TokenKind::RBrace)) {
        if (check(TokenKind::EndOfFile)) {
            error("unterminated generator body");
            ok = false;
            break;
        }
        if (match(TokenKind::Semicolon)) continue;

        // A `yield <expr>;` at the top level of the body is a STEP, and the
        // only position the subset admits. Its operand is an
        // AssignmentExpression (ECMA-262 15.5.1), so `yield a + 1` yields the
        // sum rather than yielding `a` and then adding.
        if (check(TokenKind::Identifier) && peek().text == "yield" &&
            peek(1).kind != TokenKind::Star) {
            const Token& kw = advance();
            GeneratorStep step;
            step.span = kw.span;
            const bool bare = check(TokenKind::Semicolon) || check(TokenKind::RBrace) ||
                              check(TokenKind::EndOfFile) || atLineBreak();
            if (bare) {
                // `yield;` is `yield undefined` (15.5.1 has no operand in that
                // production), and is written that way rather than left null so
                // every step below has a value to build a result from.
                auto undef = std::make_unique<UndefinedLit>();
                undef->span = kw.span;
                step.value = std::move(undef);
            } else {
                yieldRefusal_ = kValueUsedRefusal;
                step.value = parseAssign();
                yieldRefusal_ = nullptr;
                if (!step.value) {
                    ok = false;
                    break;
                }
            }
            if (!consumeSemicolon("';' after a `yield`")) {
                ok = false;
                break;
            }
            step.before = std::move(pending);
            pending.clear();
            steps.push_back(std::move(step));
            continue;
        }

        // A declaration at the top level of the body would have to survive
        // the `return` that every step ends with, and the index switch has no
        // way to carry one: the body is re-entered from the top on the next
        // call. Refusing it by name is what keeps that from being a binding
        // that silently reads `undefined` on the second step.
        if (check(TokenKind::KwConst) || check(TokenKind::KwLet) || check(TokenKind::KwVar) ||
            check(TokenKind::KwFunction) || check(TokenKind::KwClass)) {
            // Parsed FIRST and refused after, so that `const x = yield v;` —
            // the reason a program declares anything here at all — is
            // diagnosed as the yield whose value it reads rather than as the
            // declaration that reads it. Two true statements about the same
            // line; the useful one is the yield.
            const Span declSpan = peek().span;
            yieldRefusal_ = kValueUsedRefusal;
            const bool parsed = parseStatement(pending);
            yieldRefusal_ = nullptr;
            if (parsed) {
                diags_.error(declSpan,
                             "unsupported construct: a declaration at the top level of a "
                             "generator body (nothing bronze declares there can survive a "
                             "`yield`); the straight-line subset is a sequence of "
                             "`yield <expr>;` statements (docs/0026)");
            }
            ok = false;
            break;
        }
        // `return` in a generator is not the function's return: it is the
        // value of the FINAL iterator result and it terminates the walk from
        // wherever it is written. Both halves of that are outside the subset,
        // and both are named where they are written — the check inside
        // `parseReturn` catches the ones nested in a statement.
        if (check(TokenKind::KwReturn)) {
            error(peek(1).kind == TokenKind::Semicolon || peek(1).newlineBefore
                      ? "unsupported construct: `return;` in a generator (it would end the walk "
                        "early, and bronze's step index only counts forwards); the straight-line "
                        "subset is a sequence of `yield <expr>;` statements (docs/0026)"
                      : "unsupported construct: `return <expr>;` in a generator (its value is the "
                        "`value` of the final `{ done: true }` result, which bronze does not "
                        "carry); the straight-line subset is a sequence of `yield <expr>;` "
                        "statements (docs/0026)");
            ok = false;
            break;
        }

        // Anything else: it becomes part of the segment it is written in, and
        // may not contain a `yield` — which is what `yieldRefusal_` turns
        // into a message naming the construct it would have been inside.
        yieldRefusal_ = refusalForStatement(peek().kind);
        if (yieldRefusal_ == nullptr) yieldRefusal_ = kValueUsedRefusal;
        const bool parsed = parseStatement(pending);
        yieldRefusal_ = nullptr;
        if (!parsed) {
            ok = false;
            break;
        }
    }

    inGeneratorBody_ = savedInBody;
    yieldRefusal_ = savedRefusal;
    if (!ok) return false;
    if (!expect(TokenKind::RBrace, "'}' to close a generator body")) return false;
    std::vector<StmtPtr> trailing = std::move(pending);

    // --- the desugared body -------------------------------------------------
    std::vector<StmtPtr> nextBody;
    for (size_t i = 0; i < steps.size(); ++i) {
        auto& step = steps[i];
        std::vector<StmtPtr> guarded;
        // The index advances BEFORE the step runs, so an exception thrown out
        // of a yielded expression leaves the walk past that step rather than
        // repeating it for ever.
        guarded.push_back(setStep(stepName, static_cast<double>(i + 1), step.span));
        for (auto& s : step.before) guarded.push_back(std::move(s));
        guarded.push_back(
            returnStmt(iterResult(std::move(step.value), /*done=*/false, step.span), step.span));
        nextBody.push_back(
            stepGuard(stepName, static_cast<double>(i), std::move(guarded), step.span));
    }
    if (!trailing.empty()) {
        // Statements after the last yield run on the call that reports
        // `done`, once: the guard is what stops a second `next()` running
        // them again.
        std::vector<StmtPtr> guarded;
        guarded.push_back(setStep(stepName, static_cast<double>(steps.size() + 1), bodySpan));
        for (auto& s : trailing) guarded.push_back(std::move(s));
        nextBody.push_back(
            stepGuard(stepName, static_cast<double>(steps.size()), std::move(guarded), bodySpan));
    }
    {
        auto undef = std::make_unique<UndefinedLit>();
        undef->span = bodySpan;
        nextBody.push_back(
            returnStmt(iterResult(std::move(undef), /*done=*/true, bodySpan), bodySpan));
    }

    auto nextFn = std::make_unique<FunctionExpr>();
    nextFn->span = bodySpan;
    nextFn->name = "gen." + qualifier + ".next";
    nextFn->isArrow = true;
    nextFn->body = std::move(nextBody);

    // 27.5.1.2: a generator object is its own iterator. Written as a method
    // rather than an arrow precisely because it wants the RECEIVER — the
    // object it is read from — and not the enclosing `this`.
    auto selfFn = std::make_unique<FunctionExpr>();
    selfFn->span = bodySpan;
    selfFn->name = "gen." + qualifier + ".@@iterator";
    {
        auto self = std::make_unique<ThisExpr>();
        self->span = bodySpan;
        selfFn->body.push_back(returnStmt(std::move(self), bodySpan));
    }

    auto iterObj = std::make_unique<ObjectLit>();
    iterObj->span = bodySpan;
    ObjectProp nextProp;
    nextProp.key = "next";
    nextProp.value = std::move(nextFn);
    iterObj->props.push_back(std::move(nextProp));
    ObjectProp selfProp;
    selfProp.key = "@@iterator";
    selfProp.value = std::move(selfFn);
    iterObj->props.push_back(std::move(selfProp));

    auto stepDecl = std::make_unique<VarDecl>();
    stepDecl->span = bodySpan;
    stepDecl->name = stepName;
    stepDecl->init = numberLit(0, bodySpan);

    fn.body.clear();
    fn.body.push_back(std::move(stepDecl));
    fn.body.push_back(returnStmt(std::move(iterObj), bodySpan));
    fn.span.end = peek().span.begin;
    return true;
}

}  // namespace bronze
