#pragma once
#include <memory>
#include <vector>

#include "ast/ast.h"
#include "lex/token.h"
#include "support/diagnostics.h"

namespace bronze {

// Recursive-descent parser over the token stream. One method per grammar
// production; expression parsing is precedence-climbing. Errors are
// diagnosed and abort the current production — there is no error recovery
// yet (a parse with errors returns nullptr and the caller must check the
// sink; partial ASTs are never returned).
class Parser {
public:
    // `fileId` is the module graph's numbering of this file. It is used for one
    // thing: qualifying the IL symbol the parser invents for an object-literal
    // method, whose ordinal is per parser and therefore per file, so two files
    // each holding `{ next() {} }` would otherwise agree on `obj.0.next`. File
    // 0 — a single-file build's only file, and a graph's entry — is spelled
    // exactly as it was before modules existed.
    Parser(std::vector<Token> tokens, DiagnosticSink& diags, uint16_t fileId = 0)
        : tokens_(std::move(tokens)), diags_(diags), fileId_(fileId) {}

    // Parses a whole module (a file). Consumes ALL input: trailing tokens
    // after the last declaration are a hard error. Two outcomes and no third
    // — a module that consumed every token, or a diagnostic — because a
    // parser that returns what it managed and stays quiet loses the rest of
    // the file silently. Pinned by "a module is never returned with input
    // left unconsumed" in tests/parse.
    //
    // `forceStrict` is ECMA-262 11.2.2, "module code is always strict mode
    // code". The parser cannot decide that for itself — what makes a file
    // module code is how it was REACHED, and the first `import` is a statement
    // in, far too late for the early errors that depend on the mode — so the
    // module loader decides and says so here. A `"use strict"` prologue still
    // selects strict on its own; this only takes away the option of sloppy.
    std::unique_ptr<ast::Module> parseModule(std::string name, bool forceStrict = false);

private:
    std::vector<Token> tokens_;
    DiagnosticSink& diags_;
    uint16_t fileId_ = 0;
    size_t pos_ = 0;
    // Which class a `super` in the body being parsed belongs to, and whether
    // there is one at all. A class body is the only place `super` is legal, and
    // the parent it names is known here and nowhere later.
    std::string currentClassSuper_;
    bool inClassMethod_ = false;
    // Whether the operand `parseUnaryPrefix` just produced is an
    // unparenthesized unary expression, which is the one thing `**` may not
    // have on its left (ECMA-262 13.6: the left operand is an
    // UpdateExpression, so `++a ** 2` is fine and `-a ** 2` is not). Set at
    // every return of that function, read only by the `**` rung of
    // `parseBinary`.
    bool lastOperandIsUnary_ = false;
    // Ordinal of the next object-literal method, used only to build an IL
    // function name for it. Lowering keys `functionIndices_` on that name, so
    // a method called `next` must NOT be named `next` — a method name is a
    // property key and never a binding — and two literals in one module must
    // not agree on a name either, or the second's body would silently replace
    // the first's.
    size_t objectMethodOrdinal_ = 0;
    // Whether the statement about to be parsed is directly in the module's
    // body. `import` and `export` are legal there and nowhere else (ECMA-262
    // 16.2), and the parser is the only place that still knows the
    // difference — by the time the linker sees a statement list it cannot
    // tell a module body from a block.
    bool atModuleTopLevel_ = false;
    // Generator state. `yield` is not a reserved word — it is contextual, and
    // only inside a generator BODY — so this one flag decides whether the
    // identifier spelled `yield` is an operator here or an ordinary name.
    bool inGeneratorBody_ = false;
    // Ordinal of the next generator, for the temporaries `ast::liftYields`
    // declares in its body. Same reasoning as `objectMethodOrdinal_`, including
    // the file qualification: two files' first generators must not name one
    // temporary.
    size_t generatorOrdinal_ = 0;
    // Async state, the exact shape of the generator pair above: `await` is not
    // a reserved word — it is contextual, and only inside an async function
    // BODY — so this one flag decides whether the identifier spelled `await`
    // is an operator here or an ordinary name. The ordinal numbers the
    // temporaries the await lift declares, kept apart from the generators'
    // (`async.N.` vs `gen.N.`) so a reader of a lifted body can tell which
    // machine a temporary belongs to.
    bool inAsyncBody_ = false;
    size_t asyncOrdinal_ = 0;
    // Whether the code under the cursor is STRICT (ECMA-262 11.2.2).
    // Strictness is a property of a Script or a function body, decided by that
    // body's Directive Prologue and fixed for good at parse time — so a flag
    // that follows the cursor is the whole of it: everything written inside a
    // strict body is parsed with it set, which is exactly what "a function
    // declared inside strict code is strict" means. Nothing later can ask the
    // question, which is why the answer is written onto the AST nodes here.
    bool strict_ = false;

    const Token& peek(size_t ahead = 0) const;
    const Token& advance();
    bool check(TokenKind kind) const { return peek().kind == kind; }
    bool match(TokenKind kind);
    const Token* expect(TokenKind kind, const char* what);
    // An IdentifierName (ECMA-262 12.7.1), which is an identifier OR a
    // reserved word. `o.delete`, `o.new` and `{ default: 1 }` are all legal
    // JavaScript and all of them reached bronze's lexer as keyword tokens, so
    // an `expect(Identifier, ...)` after a `.` rejected the ordinary spelling
    // of `Map.prototype.delete`. Only the position decides: a reserved word
    // is a name only after `.`, `?.` or in a property-key slot.
    const Token* expectPropertyName(const char* what);
    static bool isIdentifierName(TokenKind kind);
    void error(const char* message);
    // Consumes a statement's terminating semicolon, or inserts one where
    // ECMA-262 12.10 says the program means one. Every statement terminator
    // goes through here; the semicolons that are punctuation of a production —
    // the two in a `for` header — go through expect() instead.
    bool consumeSemicolon(const char* what);
    // Whether a line terminator precedes the next token. The restricted
    // productions (`return`, `throw`, `break`, `continue`, postfix `++`/`--`)
    // are the only readers.
    bool atLineBreak() const { return peek().newlineBefore; }

    // Appends to a statement LIST rather than returning one node, because two
    // productions do not map to exactly one statement: `let a = 1, b = 2` is
    // several (each declarator is its own binding, and wrapping them in a
    // BlockStmt would give them a scope they do not have), and the empty
    // statement is none at all. Returns false on a diagnosed error.
    bool parseStatement(std::vector<ast::StmtPtr>& out);
    // `defaultName` is used only for `export default function () {}`, the
    // one production in which a DECLARATION may be anonymous. Everything
    // downstream identifies a function by its name, so it gets one.
    ast::StmtPtr parseFunctionDecl(bool isExported, const std::string& defaultName = "");
    // --- parser_module.cpp: `import` and `export` ------------- Both append
    // rather than return: `export const a = 1, b = 2` is two declarations plus
    // the record of what they export, and a side-effect `import` is one node
    // with no bindings at all.
    bool parseImportDecl(std::vector<ast::StmtPtr>& out);
    bool parseExportDecl(std::vector<ast::StmtPtr>& out);
    // The `from "spec"` tail, with the cursor on `from`. False on a
    // diagnosed error.
    bool parseFromClause(std::string& outSpecifier, Span& outSpan);
    // The names a statement declares, for `export <declaration>` — which
    // exports every name the declaration binds, patterns included.
    static void declaredNamesOf(const ast::Stmt& stmt, std::vector<std::string>& out);
    // One `VarDecl` per declarator of the BindingList (ECMA-262 14.3.1),
    // appended in source order. `isStatement` is false inside a `for` header,
    // where the declaration is followed by the header's own semicolon and ASI
    // must not apply.
    bool parseVarDecl(std::vector<ast::StmtPtr>& out, bool isStatement = true);
    ast::StmtPtr parseReturn();
    ast::StmtPtr parseIf();
    ast::StmtPtr parseWhile();
    ast::StmtPtr parseDoWhile();
    ast::StmtPtr parseFor();
    ast::StmtPtr parseBreak();
    ast::StmtPtr parseContinue();
    ast::StmtPtr parseSwitch();
    // `label: statement`. Separate from the statement it fronts because the
    // label binds nothing and produces no value — it names a jump target, and
    // only for the statement it wraps (ECMA-262 14.13).
    ast::StmtPtr parseLabeled();
    // The binding target of a `for-in` / `for-of` head, which the two forms
    // spell identically: one declaration keyword and one name or pattern.
    struct ForBindingHead {
        bool isConst = false;
        bool isLet = false;
        bool isVar = false;
        std::string name;
        ast::PatternPtr pattern;
    };
    bool parseForBindingHead(ForBindingHead& head, bool hasDecl = true);
    ast::StmtPtr parseTry();
    ast::StmtPtr parseThrow();
    // --- parser_class.cpp: class bodies and private elements --------------
    // `defaultName`, as for parseFunctionDecl: `export default class {}`.
    ast::StmtPtr parseClass(const std::string& defaultName = "");
    ast::ExprPtr parseClassExpr();
    bool parseClassBodyCommon(const std::string& name, const std::string& superName,
                              std::vector<ast::ClassMethod>& methods, Span span);
    // One class body's private names. ECMA-262 15.7.1 states BOTH private-name
    // early errors over the whole ClassBody rather than over one element: a
    // duplicate is a duplicate wherever the two spellings are, and a reference
    // may stand ABOVE the declaration it resolves to (`m() { return this.#x }
    // #x = 1`). So the declared names are collected by a scan of the body's
    // tokens before any member is parsed, and the reference check consults
    // that set — which is also what makes an inner class's reference to an
    // OUTER private name resolve, since the stack is searched innermost first.
    struct PrivateNameScope {
        // Every name the body declares, `#` included. From the pre-scan, so it
        // is complete before the first member is parsed.
        std::vector<std::string> declared;
        // How each name is bound, filled in as the members are parsed. 15.7.1
        // admits one repetition and one only — a getter and a setter of the
        // same name, both static or both not — so counting the three kinds
        // separately is exactly the rule, and one flag would not be.
        struct Binding {
            std::string name;
            uint32_t getters = 0;
            uint32_t setters = 0;
            uint32_t others = 0;  // fields, methods, and anything else
            bool isStatic = false;
            Span first;
        };
        std::vector<Binding> bindings;
    };
    std::vector<PrivateNameScope> privateScopes_;
    // The declared names of the class body whose `{` is at `braceIndex`,
    // collected by walking its tokens. A `#x` at the body's OWN brace depth is
    // a declaration unless it is the `.#x` of a reference or the `#x` of
    // `#x in o`; anything deeper belongs to a nested body and is that body's
    // scan to make.
    void scanPrivateDeclarations(size_t braceIndex, PrivateNameScope& scope) const;
    // Records how `name` is bound in the innermost open class body, reporting
    // 15.7.1's duplicate error against `span`. False on a diagnosed error.
    bool declarePrivateName(const std::string& name, ast::AccessorKind accessor, bool isStatic,
                            Span span);
    // Is `name` declared by any class body currently open? The reference half
    // of 15.7.1 (AllPrivateIdentifiersValid).
    bool privateNameInScope(const std::string& name) const;
    // `.#x` / `?.#x`, with the `#x` token current. Appends the private member
    // link to `expr` and checks the reference. False on a diagnosed error.
    bool parsePrivateMemberLink(ast::ExprPtr& expr, bool optional);
    // `#x` where an expression may start, which 13.10.1 admits in exactly one
    // position: the left operand of `in`. Null on a diagnosed error.
    ast::ExprPtr parsePrivateNameOperand();
    ast::ExprPtr parseSuper();
    bool parseParams(std::vector<ast::Param>& out);
    // `get k() {}` / `set k(v) {}`, with the `get`/`set` already consumed. One
    // copy for object literals and class bodies, because the only thing that
    // differs between them is the enumerability the RUNTIME gives the result —
    // the syntax is identical, including the arity rules ECMA-262 15.4.1 puts
    // on each half. Null on error.
    std::unique_ptr<ast::FunctionExpr> parseAccessorMember(ast::AccessorKind kind,
                                                           std::string& outName,
                                                           ast::ExprPtr* outKeyExpr = nullptr);
    // `m(params) { body }`, with the NAME already consumed and the cursor on
    // the '('. The tail is the same production in an object literal and in a
    // class body (ECMA-262 15.4 MethodDefinition), so there is one copy;
    // what differs is what the caller does with the result.
    std::unique_ptr<ast::FunctionExpr> parseMethodTail(const std::string& name, Span nameSpan);

    // --- parser_generator.cpp: generators ---------------------- The parameter
    // list and body of a generator, with the cursor on the '(' and the `*`
    // already consumed. `fn` comes back holding an ordinary body with `yield`
    // nodes in it, lifted so that every suspension stands at a statement
    // boundary. False on a diagnosed error.
    bool parseGeneratorTail(ast::FunctionExpr& fn);
    // `[ Symbol.iterator ]` as a class member name, the only computed key
    // bronze reads there, and the only one three.js's generators use. Returns
    // the KEY EXPRESSION with the cursor past the `]`, or null with the cursor
    // unmoved when the bracketed key is anything else.
    ast::ExprPtr matchSymbolIteratorKey();
    // Saves and restores the generator AND async state across a nested
    // function body: a `yield` inside a function written inside a generator
    // belongs to that function, which is not a generator, so it is an ordinary
    // identifier there and `return` is an ordinary return — and `await` inside
    // a function written inside an async body follows the identical rule.
    // One guard for both flags because every function boundary clears both,
    // and a boundary that cleared only one would leak the other's operator
    // into a body it does not belong to.
    struct GeneratorScopeGuard {
        Parser& p;
        bool savedInBody;
        bool savedInAsync;
        explicit GeneratorScopeGuard(Parser& parser)
            : p(parser),
              savedInBody(parser.inGeneratorBody_),
              savedInAsync(parser.inAsyncBody_) {
            p.inGeneratorBody_ = false;
            p.inAsyncBody_ = false;
        }
        ~GeneratorScopeGuard() {
            p.inGeneratorBody_ = savedInBody;
            p.inAsyncBody_ = savedInAsync;
        }
    };
    // `yield`, `yield <expr>` and `yield* <expr>` under the cursor.
    // Null on a diagnosed error.
    ast::ExprPtr parseYieldExpr();

    // --- parser_async.cpp: async functions and `await` -------------------
    // Is the cursor on the `async` that MODIFIES what follows? True for
    // `async function`, `async x =>` and `async (…) =>` with no line
    // terminator after `async` (ECMA-262 15.8.1 / 15.9.1 forbid one there);
    // false for every other `async`, which stays the ordinary identifier it
    // is — `async()`, `let async = 1`, `async` alone before a newline.
    bool asyncModifiesFunction() const;
    bool asyncModifiesArrow() const;
    // `async function f() {}` as a declaration / an expression, with the
    // cursor ON `async`. Null (or false) on a diagnosed error; `async
    // function*` is refused by name inside.
    ast::StmtPtr parseAsyncFunctionDecl(bool isExported, const std::string& defaultName = "");
    ast::ExprPtr parseAsyncFunctionExpr();
    // `async x => …` / `async (a, b) => …`, cursor on `async`.
    ast::ExprPtr parseAsyncArrow();
    // The parameter list and body shared by every async form: parses the
    // body with `await` an operator, then lifts every await to a statement
    // boundary exactly as parseGeneratorTail lifts yields. Cursor on `(`.
    bool parseAsyncFnTail(ast::FunctionExpr& fn);
    // The statement-boundary lift alone, for the async arrow whose body was
    // parsed by the arrow production rather than by the tail above.
    bool liftAsyncBody(std::vector<ast::StmtPtr>& body);
    // `await <UnaryExpression>` with the cursor on `await`, inside an async
    // body only. Null on a diagnosed error.
    ast::ExprPtr parseAwaitExpr();
    // The arrow lookahead `looksLikeArrow` runs, started `offset` tokens
    // ahead of the cursor — which is what `async (…) =>` needs, since the
    // `async` itself is still under the cursor when the question is asked.
    bool looksLikeArrowFrom(size_t offset) const;
    // `async m(params) { body }` with the NAME already consumed — the async
    // MethodDefinition tail an object literal and a class body share, the
    // same seam parseMethodTail is one method for. `clearSuper` is the
    // object-literal half of that seam: a literal's method must not inherit
    // the enclosing class's `super`, a class's must keep it.
    std::unique_ptr<ast::FunctionExpr> parseAsyncMethodTail(const std::string& name,
                                                            Span nameSpan, bool clearSuper,
                                                            bool isGenerator = false);

    // --- parser_strict.cpp: the Directive Prologue and the early errors -----
    // Restores `strict_` on the way out of a body that may have raised it.
    // Strictness only ever goes UP on the way in — a `"use strict"` prologue,
    // a class body — so a sloppy function written after a strict sibling would
    // otherwise inherit its neighbour's mode.
    struct StrictScopeGuard {
        Parser& p;
        bool saved;
        explicit StrictScopeGuard(Parser& parser) : p(parser), saved(parser.strict_) {}
        ~StrictScopeGuard() { p.strict_ = saved; }
    };
    // Whether the Directive Prologue at the cursor (ECMA-262 11.2.2) selects
    // strict mode. Consumes nothing: the directives are ordinary
    // ExpressionStatements and are parsed as such afterwards. The prologue has
    // to be read BEFORE the body, because it decides which early errors the
    // body's own statements are subject to.
    bool prologueSelectsStrict() const;
    // A function BODY: a braced StatementList that has a Directive Prologue,
    // which is the one thing an ordinary block does not have. `outStrict` is
    // the strictness of the body — the enclosing code's, raised by the
    // prologue — and is what the FunctionExpr/FunctionDecl node records.
    std::vector<ast::StmtPtr> parseFunctionBody(bool& outStrict);
    // The strict-mode early errors, each named after the rule it enforces.
    // All are no-ops in sloppy code, where the construct is legal.
    //
    // A BINDING name (12.7.2 / 13.15.1): `eval` and `arguments` may not be
    // bound, and the nine future reserved words may not be used at all.
    // `role` names the position for the diagnostic ("parameter", "variable").
    bool checkStrictBindingName(std::string_view name, Span span, const char* role);
    // An identifier REFERENCE: the future reserved words only — `eval` and
    // `arguments` are legal to read, and illegal only as a target.
    bool checkStrictIdentifierReference(std::string_view name, Span span);
    // An assignment or update TARGET (13.15.1): `eval` and `arguments`.
    bool checkStrictAssignmentTarget(const ast::Expr& target);
    // A parameter list, once the body's strictness is known: duplicates
    // (15.1.2 / 15.2.1), and every name through checkStrictBindingName. Run
    // after the body because a function's own `"use strict"` is what makes its
    // parameter list subject to the rule.
    bool checkStrictParams(const std::vector<ast::Param>& params, bool bodyStrict);

    // --- parser_pattern.cpp: binding patterns ---------------- A pattern where
    // the grammar spells one: a declarator, a parameter, a for-of head. Null on
    // a diagnosed error.
    ast::PatternPtr parsePattern();
    ast::PatternPtr parseArrayPattern();
    ast::PatternPtr parseObjectPattern();
    // The target half of one element — a name or a nested pattern — filled
    // into `elem`. False on a diagnosed error.
    bool parsePatternTarget(ast::PatternElement& elem);
    // ECMA-262 13.15.5's refinement: an ArrayLiteral or ObjectLiteral on the
    // left of `=` was covering an AssignmentPattern all along, and this is
    // the point the `=` reveals it. The parser owns both trees, so the nodes
    // MOVE across rather than being copied or re-parsed.
    ast::PatternPtr patternFromLiteral(ast::ExprPtr expr);
    // The token index just past the binding target that starts at `at` — a
    // name, or a bracketed/braced pattern. `for (const [k, v] of ...)` needs
    // it: the token that decides between a `for`, a `for-in` and a `for-of`
    // sits after a group of unbounded length.
    size_t skipBindingTarget(size_t at) const;
    std::vector<ast::StmtPtr> parseBlock();
    std::vector<ast::StmtPtr> parseBlockOrSingleStmt();
    std::string parseTypeAnnotation();
    // The characters a string literal denotes, escapes resolved (see the
    // definition: the lexer finds the literal's end, the parser decides what
    // it means).
    std::string decodeStringLiteral(std::string_view raw, Span span);
    // The Number a NumericLiteral denotes, radix prefixes and separators
    // resolved (see the definition: the lexer finds the literal's end, the
    // parser decides what it means). False on a diagnosed error.
    bool decodeNumericLiteral(std::string_view raw, Span span, double& out);
    // Does this NumericLiteral token carry the BigInt suffix? The one question
    // that decides WHICH node a numeric token becomes, asked in the two places
    // that build one so neither can guess.
    static bool hasBigIntSuffix(std::string_view raw);
    // The digits a BigIntLiteral denotes, cleaned for StringToBigInt: `n` and
    // separators removed, radix prefix kept. False on a diagnosed error — and
    // the errors are the whole point, since 12.9.3's BigIntLiteralSuffix
    // attaches only to an INTEGER (`1.5n` and `1e3n` are not literals) and
    // never to a legacy octal (`0123n`).
    bool decodeBigIntLiteral(std::string_view raw, Span span, std::string& out);
    ast::ExprPtr parseTemplateLiteral();
    // `/ab+/gi`. Splits the one token into its pattern and its flags and
    // COMPILES the pattern, so a malformed regular expression is a compile
    // error at the position it was written rather than a hard error the first
    // time the line runs.
    ast::ExprPtr parseRegExpLiteral();
    bool looksLikeArrow() const;
    ast::ExprPtr parseArrowFunction();

    // The expression grammar, loosest production first. `parseExpr` is
    // ECMA-262's *Expression* and admits the comma operator; `parseAssign` is
    // *AssignmentExpression* and does not. Every position the spec spells
    // AssignmentExpression — an argument, an array element, a property value, a
    // declarator initializer, a ternary arm, the right side of an assignment —
    // calls the latter, which is what keeps `f(a, b)` a two-argument call.
    ast::ExprPtr parseExpr();
    ast::ExprPtr parseAssign();
    ast::ExprPtr parseConditional();
    ast::ExprPtr parseBinary(int minPrecedence);
    ast::ExprPtr parseUnaryPrefix();
    ast::ExprPtr parseUnaryPostfix();
    ast::ExprPtr parsePostfixOps(ast::ExprPtr expr);
    // One `.name` / `[expr]` link, appended in place (see the definition:
    // the suffix chain and a `new` callee share it).
    bool parseMemberLink(ast::ExprPtr& expr);
    ast::ExprPtr parseNew();
    ast::ExprPtr parseNewCore();
    ast::ExprPtr parseNewCallee();
    ast::ExprPtr parsePrimary();
    // Parses "expr, expr, ..." up to and including the closing ')' (the
    // caller has already consumed the opening '('). False on error.
    bool parseArgumentList(std::vector<ast::ExprPtr>& args);
    ast::ExprPtr parseObjectLit();
    ast::ExprPtr parseArrayLit();
    ast::ExprPtr parseFunctionExpr();
};

}  // namespace bronze
