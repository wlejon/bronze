#pragma once
#include <memory>
#include <string>
#include <vector>

#include "support/source.h"

namespace bronze::ast {

class Visitor;

struct Node {
    bronze::Span span;
    virtual ~Node() = default;
    virtual void accept(Visitor& v) const = 0;
};

// ---- Expressions -----------------------------------------------------------

struct Expr : Node {
    // Whether the source wrapped this expression in parentheses. Two rules
    // in ECMA-262 are stated over the *unparenthesized* form and cannot be
    // checked without it: `a ?? b || c` is a SyntaxError while
    // `a ?? (b || c)` is not, and `-2 ** 2` is a SyntaxError while
    // `(-2) ** 2` is not. Nothing else reads it, and it is deliberately not
    // dumped: parentheses change what the tree IS, and the tree is what the
    // dump shows.
    bool parenthesized = false;
};
using ExprPtr = std::unique_ptr<Expr>;

// ---- Binding patterns (docs/0017) ------------------------------------------
//
// A pattern is what stands where a binding name would: in a declaration, in a
// parameter, in a for-of head, and — after the cover-grammar refinement at the
// `=` — on the left of a destructuring assignment. It nests, so it is a tree
// and not a list.

struct BindingPattern;
using PatternPtr = std::unique_ptr<BindingPattern>;

// One element of a binding pattern: where its value is read from, what it is
// bound to, and the default that fires when the read produced `undefined`.
struct PatternElement {
    // The target. Exactly one of these is meaningful.
    std::string name;
    PatternPtr pattern;
    // OBJECT patterns only: which property this element reads. `key` is a
    // written name and `keyExpr` a computed `[e]`; the two are never both
    // meaningful. An ARRAY pattern reads by position and uses neither.
    std::string key;
    ExprPtr keyExpr;
    // `= expr`. Evaluated only when the read produced `undefined` — not on
    // `null`, and not at all otherwise, so its side effects are observable
    // evidence of whether it fired (docs/0017 decision 1).
    ExprPtr defaultValue;
    // `...rest`, which is always last and always binds a fresh container:
    // an array for an array pattern, an object for an object one.
    bool isRest = false;
    Span span;
};

struct BindingPattern {
    bool isObject = false;  // false: an array pattern, read by position
    std::vector<PatternElement> elements;
    Span span;
};

// Every name a pattern binds, in source order. Scope planning asks a pattern
// the same question it asks a plain declaration — which names appear here —
// and there is one answer to it rather than one per caller.
std::vector<std::string> patternBoundNames(const BindingPattern& pattern);

struct NumberLit final : Expr {
    double value = 0;
    void accept(Visitor& v) const override;
};

// `...expr` in an argument list, an array literal or an object literal. Its
// own node rather than a flag on the list, because it is not an expression
// that produces one value: it contributes zero or more of them to the list it
// sits in, so every position that lowers a list has to decide what to do with
// it and none may treat it as an operand (docs/0017 decision 3).
struct SpreadElement final : Expr {
    ExprPtr argument;
    void accept(Visitor& v) const override;
};

struct StringLit final : Expr {
    std::string value;  // decoded (quotes/escapes resolved)
    void accept(Visitor& v) const override;
};

// A template literal. `quasis` is always one longer than `exprs`: the
// pieces alternate, starting and ending with a (possibly empty) piece, so
// `${x}` is ["", ""] with one expression. The pieces are DECODED, like any
// other string literal.
struct TemplateLit final : Expr {
    std::vector<std::string> quasis;
    std::vector<ExprPtr> exprs;
    void accept(Visitor& v) const override;
};

// `/ab+/gi`. The pattern is held VERBATIM — a regular expression literal's
// body is not a string literal and its escapes are the pattern grammar's, so
// `\d` here is two characters and stays two (docs/0024 decision 2).
//
// It is a literal in the grammar and an object at run time: every evaluation
// produces a fresh RegExp with its own `lastIndex`, which is why lowering
// spells it as a construction rather than as a constant.
struct RegExpLit final : Expr {
    std::string pattern;
    std::string flags;
    void accept(Visitor& v) const override;
};

struct Ident final : Expr {
    std::string name;
    void accept(Visitor& v) const override;
};

struct BoolLit final : Expr {
    bool value = false;
    void accept(Visitor& v) const override;
};

struct NullLit final : Expr {
    void accept(Visitor& v) const override;
};

struct UndefinedLit final : Expr {
    void accept(Visitor& v) const override;
};

// `this`. Not an identifier: it resolves to the receiver the caller passed
// (docs/0008 decision 3), never to a binding in scope.
struct ThisExpr final : Expr {
    void accept(Visitor& v) const override;
};

// Which half of an accessor a property definition writes, or neither.
enum class AccessorKind { None, Getter, Setter };

enum class UnaryOp {
    Not, Negate, Posate, PreInc, PreDec, PostInc, PostDec,
    // `~` is ToInt32 then a one's complement; `typeof` yields one of six
    // strings; `void` evaluates its operand and yields undefined (docs/0015).
    BitNot, TypeOf, Void,
    // `delete o.k` — a *reference* operator: its operand is not evaluated
    // as a value, so lowering dispatches on the operand's own node kind
    // rather than lowering it first (docs/0019 decision 2).
    Delete
};
const char* unaryOpName(UnaryOp op);

struct Unary final : Expr {
    UnaryOp op;
    ExprPtr operand;
    void accept(Visitor& v) const override;
};

enum class BinaryOp {
    Add, Sub, Mul, Div, Mod, Less, Greater, LessEqual, GreaterEqual,
    Eq, StrictEq, Ne, StrictNe, Assign, PlusAssign, MinusAssign,
    StarAssign, SlashAssign, PercentAssign, LogicalAnd, LogicalOr, NullishCoalescing,
    // docs/0015. The bitwise and shift operators are int32 operations on
    // ToInt32'd operands; `Exp` is the one right-associative binary operator;
    // `In` and `InstanceOf` are relational; `Comma` evaluates its left
    // operand for effect and yields its right.
    BitAnd, BitOr, BitXor, Shl, Shr, UShr, Exp, In, InstanceOf, Comma,
    AmpAssign, PipeAssign, CaretAssign, ShlAssign, ShrAssign, UShrAssign, ExpAssign
};
const char* binaryOpName(BinaryOp op);

// Does this operator WRITE its left operand? Three passes ask — the
// assigned-variable scan that sizes SSA joins, inference's flow analysis,
// and lowering — and they must agree, so there is one table and not three
// lists. A compound operator missing from a copy of the list is not a build
// error, it is a variable that silently stops taking part in a loop join.
bool isAssignOp(BinaryOp op);
bool isCompoundAssignOp(BinaryOp op);
// The plain operator a compound assignment applies (`+=` -> `+`). Returns
// `op` itself for anything that is not one.
BinaryOp compoundAssignBase(BinaryOp op);

struct Binary final : Expr {
    BinaryOp op;
    ExprPtr lhs;
    ExprPtr rhs;
    void accept(Visitor& v) const override;
};

struct Ternary final : Expr {
    ExprPtr condition;
    ExprPtr thenExpr;
    ExprPtr elseExpr;
    void accept(Visitor& v) const override;
};

// `?.` on the three link forms. The flag says this LINK is optional; whether
// a link is part of a longer chain that its short circuit must skip is a
// question about the tree, answered by `optionalChainRoot` below, because
// ECMA-262 13.3.9 short-circuits the whole OptionalChain and not one link
// (docs/0018 decision 4).
struct MemberAccess final : Expr {
    ExprPtr object;
    std::string property;
    bool optional = false;
    void accept(Visitor& v) const override;
};

struct IndexAccess final : Expr {
    ExprPtr object;
    ExprPtr index;
    bool optional = false;
    void accept(Visitor& v) const override;
};

struct Call final : Expr {
    ExprPtr callee;
    std::vector<ExprPtr> args;
    bool optional = false;
    void accept(Visitor& v) const override;
};

// The base of `expr`'s optional chain, or null when `expr` contains no `?.`
// that this node's evaluation would short-circuit. A parenthesized
// subexpression ENDS a chain (it is a PrimaryExpression, not an
// OptionalChain), so the walk stops there — which is the whole difference
// between `a?.b.c` and `(a?.b).c`.
bool containsOptionalLink(const Expr& expr);

struct NewExpr final : Expr {
    std::string callee;  // constructor name; only identifier callees are supported
    std::vector<ExprPtr> args;
    void accept(Visitor& v) const override;
};

// `super(...)` — the parent constructor run on the current receiver, and
// `super.m` — a lookup that starts at the parent prototype but is called
// with the current receiver. Both carry the parent class's NAME, resolved
// by the parser from the enclosing class: the home object is a compile-time
// constant per method, not a runtime lookup (docs/0012 decision 5). Carrying
// the name is also what makes the parent visible to capture analysis, which
// otherwise sees a method body that mentions no such variable.
struct SuperCall final : Expr {
    std::string baseName;
    std::vector<ExprPtr> args;
    void accept(Visitor& v) const override;
};

struct SuperMember final : Expr {
    std::string baseName;
    std::string property;
    void accept(Visitor& v) const override;
};

// `[a, b] = pair` — a destructuring ASSIGNMENT, which writes bindings that
// already exist rather than making new ones. Its own node because it lowers
// nothing like `Binary{Assign}`: there is no single target to evaluate, and
// the whole right side is read before any target is written, which is what
// makes `[a, b] = [b, a]` a swap (docs/0017 decision 5).
struct DestructuringAssign final : Expr {
    PatternPtr pattern;
    ExprPtr value;
    void accept(Visitor& v) const override;
};

// ---- Statements / declarations ---------------------------------------------

struct Stmt : Node {};
using StmtPtr = std::unique_ptr<Stmt>;

struct BlockStmt final : Stmt {
    std::vector<StmtPtr> stmts;
    void accept(Visitor& v) const override;
};

struct ObjectProp {
    // The property name, when the source wrote one. A COMPUTED key —
    // `{ [e]: v }` — has no name until `keyExpr` is evaluated and
    // ToPropertyKey'd, so `key` is empty and means nothing there; the two
    // fields are never both meaningful.
    std::string key;
    ExprPtr keyExpr;  // null unless the key is computed
    // The value, or — for `{ ...src }` — a `SpreadElement` holding the source.
    ExprPtr value;
    // `{ x = 1 }`, a CoverInitializedName: legal only once the literal is
    // refined into a destructuring pattern, and never as a literal in its own
    // right. `value` is then the `x = 1` assignment the cover grammar parsed,
    // which is exactly what the refinement needs and what lowering must
    // refuse (docs/0017 decision 5).
    bool coverInitialized = false;
    // `get k() {}` / `set k(v) {}`. The property is then an ACCESSOR whose
    // `value` is the getter or the setter function; the two halves of one
    // name are one property, which is a fact only the runtime can enforce
    // (docs/0019 decision 4).
    AccessorKind accessor = AccessorKind::None;
    bool computed() const { return keyExpr != nullptr; }
};

struct ObjectLit final : Expr {
    std::vector<ObjectProp> props;
    void accept(Visitor& v) const override;
};

struct ArrayLit final : Expr {
    // A `SpreadElement` among them contributes its source's elements rather
    // than one value, so the literal's length is not its element count.
    std::vector<ExprPtr> elements;
    void accept(Visitor& v) const override;
};

struct Param {
    // The parameter's name, or empty when the parameter is a `pattern`.
    std::string name;
    std::string typeAnnotation;
    // `= expr`, evaluated at CALL time on every call that omits the argument,
    // with the parameters to its left already bound (docs/0017 decision 1).
    ExprPtr defaultValue;
    PatternPtr pattern;
    // `...rest` — always the last parameter, and always a real array.
    bool isRest = false;
    Span span;
};

struct FunctionExpr final : Expr {
    std::string name;
    std::vector<Param> params;
    std::string returnType;
    std::vector<StmtPtr> body;
    // An arrow function. The same node because it is the same thing —
    // a closure value — with one semantic difference that matters:
    // `this` inside it is the ENCLOSING function's receiver, captured
    // like any other free variable, rather than one the caller supplies
    // (docs/0012 decision 3). An expression body is stored as a single
    // synthesized `return`.
    bool isArrow = false;
    void accept(Visitor& v) const override;
};

struct VarDecl final : Stmt {
    bool isConst = false;
    bool isVar = false;
    std::string name;            // empty when the declarator is a `pattern`
    // `const [a, b] = pair`. The binding target is a pattern rather than one
    // name, which is why `name` and this are never both meaningful.
    PatternPtr pattern;
    std::string typeAnnotation;  // raw text for now; the type system owns this later
    ExprPtr init;                // may be null (let without initializer)
    void accept(Visitor& v) const override;
};

struct ReturnStmt final : Stmt {
    ExprPtr value;  // may be null
    void accept(Visitor& v) const override;
};

struct ExprStmt final : Stmt {
    ExprPtr expr;
    void accept(Visitor& v) const override;
};

struct IfStmt final : Stmt {
    ExprPtr condition;
    std::vector<StmtPtr> thenBody;
    std::vector<StmtPtr> elseBody;
    void accept(Visitor& v) const override;
};

struct WhileStmt final : Stmt {
    ExprPtr condition;
    std::vector<StmtPtr> body;
    void accept(Visitor& v) const override;
};

struct DoWhileStmt final : Stmt {
    std::vector<StmtPtr> body;
    ExprPtr condition;
    void accept(Visitor& v) const override;
};

struct ForStmt final : Stmt {
    // A LIST, because `for (let i = 0, j = n; ...)` declares two bindings and
    // both belong to the loop's own scope. One `VarDecl` per declarator, or a
    // single `ExprStmt`, or empty. Not a `BlockStmt` holding them: a block
    // would give the header a scope of its own, and the header's bindings are
    // the loop's, visible to the condition, the update and the body.
    std::vector<StmtPtr> init;
    ExprPtr condition; // optional
    ExprPtr update;    // optional
    std::vector<StmtPtr> body;
    void accept(Visitor& v) const override;
};

struct BreakStmt final : Stmt {
    std::string label;
    void accept(Visitor& v) const override;
};

struct ContinueStmt final : Stmt {
    std::string label;
    void accept(Visitor& v) const override;
};

// One `case e:` or `default:` clause. `test` is null for the default clause,
// which may sit anywhere in the list: where it is written decides what falls
// through into it and out of it, and only whether any `case` matched decides
// whether it is selected (ECMA-262 14.12.4).
struct SwitchCase {
    ExprPtr test;  // null: the `default` clause
    std::vector<StmtPtr> body;
    Span span;
};

struct SwitchStmt final : Stmt {
    ExprPtr discriminant;
    std::vector<SwitchCase> cases;
    void accept(Visitor& v) const override;
};

// `for (const k in object) body`. The same head shape as ForOfStmt, and for
// the same reason: the binding is per-iteration, so a closure made in the
// body captures that iteration's key. What differs is entirely in what is
// walked — the enumerable string keys of the object AND of its prototypes,
// snapshotted before the first iteration (docs/0018 decision 1).
struct ForInStmt final : Stmt {
    std::string name;  // empty when the head destructures
    PatternPtr pattern;
    bool isConst = false;
    bool isLet = false;
    bool isVar = false;
    ExprPtr object;
    std::vector<StmtPtr> body;
    void accept(Visitor& v) const override;
};

// `label: statement`. A label is not a binding: it names a jump target for
// the `break`/`continue` inside the statement it fronts and nothing else, so
// it has no scope beyond that statement and two sibling statements may carry
// the same one (ECMA-262 14.13).
struct LabeledStmt final : Stmt {
    std::string label;
    StmtPtr body;
    void accept(Visitor& v) const override;
};

// `for (const x of iterable) body`. The binding is per-iteration by
// definition — there is no "the loop variable" to share, so a closure made
// in the body captures that iteration's value (contrast ForStmt, whose
// header binding docs/0007 decision 2 diagnoses).
struct ForOfStmt final : Stmt {
    std::string name;  // empty when the head destructures
    // `for (const [k, v] of pairs)`. Bound afresh per iteration like the
    // named form, because the pattern's names are the loop's binding.
    PatternPtr pattern;
    bool isConst = false;
    bool isLet = false;
    bool isVar = false;
    ExprPtr iterable;
    std::vector<StmtPtr> body;
    void accept(Visitor& v) const override;
};

// `try { } catch (e) { } finally { }`. The three parts are separate statement
// lists rather than blocks because each is its own scope and none of them is
// an expression; `hasCatch` and `hasFinally` distinguish an absent clause from
// an empty one, which matters — `try { } finally { }` with no catch does not
// stop an exception, and `try { } catch (e) { }` does (ECMA-262 14.15).
struct TryStmt final : Stmt {
    std::vector<StmtPtr> body;
    bool hasCatch = false;
    // The CatchParameter, which 14.15.1 makes an ordinary BindingIdentifier
    // or BindingPattern — so `catch ({ message })` is legal, and `catch { }`
    // with no parameter at all is too, which is why "has a catch clause" and
    // "has a catch parameter" are two different questions here.
    bool hasCatchParam = false;
    std::string catchName;  // empty when the parameter destructures
    PatternPtr catchPattern;
    std::vector<StmtPtr> catchBody;
    bool hasFinally = false;
    std::vector<StmtPtr> finallyBody;
    void accept(Visitor& v) const override;
};

// `throw expr;`. The expression is mandatory — 14.14's restricted production
// has no fallback reading, unlike `return`, which means `return undefined`.
struct ThrowStmt final : Stmt {
    ExprPtr value;
    void accept(Visitor& v) const override;
};

// One method of a class body. The function itself is an ordinary
// `FunctionExpr`, so everything that already reasons about a closure —
// capture analysis, `this`, inference's nested-function pass — reaches a
// method without a second code path.
struct ClassMethod {
    std::string name;
    bool isStatic = false;
    bool isConstructor = false;
    // A class accessor, which differs from an object literal's in exactly
    // one attribute: ECMA-262 15.7.14 defines it non-enumerable, the same
    // rule that already keeps a method out of `for-in` (docs/0018 dec. 2).
    AccessorKind accessor = AccessorKind::None;
    std::unique_ptr<FunctionExpr> fn;
};

// A class is the constructor function plus its prototype (docs/0008); this
// node holds what lowering needs to build that, and introduces no runtime
// concept of its own (docs/0012 decision 5).
struct ClassDecl final : Stmt {
    std::string name;
    std::string superName;  // empty when the class has no `extends`
    std::vector<ClassMethod> methods;
    void accept(Visitor& v) const override;
};

struct FunctionDecl final : Stmt {
    bool isExported = false;
    std::string name;
    std::vector<Param> params;
    std::string returnType;  // raw annotation text
    std::vector<StmtPtr> body;
    void accept(Visitor& v) const override;
};

// ---- Modules (docs/0023) ----------------------------------------------------
//
// Both of these are erased by the linker: it reads them, resolves the graph,
// and builds a merged module in which no import or export node survives. They
// exist between the parser and `src/modules` and nowhere else, which is why
// their `visit` overloads are the only ones with a default — a consumer
// downstream of the linker cannot meet one.

// One binding an `import` declaration introduces. Exactly one of the three
// forms holds: a default import (`import d from`), a namespace import
// (`import * as ns from`), or a named one (`import { a as b } from`).
struct ImportSpecifier {
    std::string imported;  // the name in the exporting module; empty for a namespace
    std::string local;     // the binding this file gets
    bool isDefault = false;
    bool isNamespace = false;
    Span span;
};

struct ImportDecl final : Stmt {
    std::string specifier;  // the module specifier text, decoded
    Span specifierSpan;
    // Empty for `import "./x.js"`, which binds nothing and exists only for
    // the side effects of evaluating the module.
    std::vector<ImportSpecifier> specifiers;
    void accept(Visitor& v) const override;
};

struct ExportSpecifier {
    std::string local;     // the name in the module being exported FROM
    std::string exported;  // the name importers ask for
    Span span;
};

// Every `export` form reduces to "these local names are visible under these
// exported names", optionally from another module. `export <declaration>`
// leaves the declaration itself in the statement list as an ordinary
// declaration and adds one of these beside it — so nothing downstream has to
// know that a declaration can be wrapped, and `export const a = 1, b = 2`
// (which is two declarations) needs no special shape.
struct ExportNamesDecl final : Stmt {
    std::vector<ExportSpecifier> specifiers;
    // `export ... from './x'`. The names are then the OTHER module's, and
    // this file gets no binding for them.
    bool hasFrom = false;
    std::string fromSpecifier;
    Span fromSpan;
    // `export * from './x'` — every name the target exports except `default`,
    // decided at link time because it depends on the target's own table.
    // With `starAlias` non-empty it is `export * as ns from './x'`, which
    // exports one name bound to the target's namespace object.
    bool isStar = false;
    std::string starAlias;
    void accept(Visitor& v) const override;
};

struct Module final : Node {
    std::string name;
    std::vector<StmtPtr> body;
    void accept(Visitor& v) const override;
};

// ---- Visitor ----------------------------------------------------------------

class Visitor {
public:
    virtual ~Visitor() = default;
    virtual void visit(const NumberLit&) = 0;
    virtual void visit(const SpreadElement&) = 0;
    virtual void visit(const StringLit&) = 0;
    virtual void visit(const TemplateLit&) = 0;
    virtual void visit(const RegExpLit&) = 0;
    virtual void visit(const BoolLit&) = 0;
    virtual void visit(const NullLit&) = 0;
    virtual void visit(const UndefinedLit&) = 0;
    virtual void visit(const ThisExpr&) = 0;
    virtual void visit(const Ident&) = 0;
    virtual void visit(const Unary&) = 0;
    virtual void visit(const Binary&) = 0;
    virtual void visit(const Ternary&) = 0;
    virtual void visit(const MemberAccess&) = 0;
    virtual void visit(const IndexAccess&) = 0;
    virtual void visit(const Call&) = 0;
    // Not pure: the default walks the args (the only children), so traversal
    // visitors get the correct Call-style behavior without an override.
    // Visitors that render or transform the node must override it.
    virtual void visit(const NewExpr&) = 0;
    virtual void visit(const SuperCall&) = 0;
    virtual void visit(const SuperMember&) = 0;
    virtual void visit(const DestructuringAssign&) = 0;
    virtual void visit(const ObjectLit&) = 0;
    virtual void visit(const ArrayLit&) = 0;
    virtual void visit(const FunctionExpr&) = 0;
    virtual void visit(const BlockStmt&) = 0;
    virtual void visit(const VarDecl&) = 0;
    virtual void visit(const ReturnStmt&) = 0;
    virtual void visit(const ExprStmt&) = 0;
    virtual void visit(const IfStmt&) = 0;
    virtual void visit(const WhileStmt&) = 0;
    virtual void visit(const DoWhileStmt&) = 0;
    virtual void visit(const ForStmt&) = 0;
    virtual void visit(const BreakStmt&) = 0;
    virtual void visit(const ContinueStmt&) = 0;
    virtual void visit(const SwitchStmt&) = 0;
    virtual void visit(const ForInStmt&) = 0;
    virtual void visit(const LabeledStmt&) = 0;
    virtual void visit(const ForOfStmt&) = 0;
    virtual void visit(const TryStmt&) = 0;
    virtual void visit(const ThrowStmt&) = 0;
    virtual void visit(const ClassDecl&) = 0;
    virtual void visit(const FunctionDecl&) = 0;
    // Not pure, and deliberately empty. The linker erases both nodes before
    // inference or lowering runs (docs/0023 decision 1), so a visitor written
    // for a merged module can never be handed one; requiring every existing
    // visitor to write an override for a node it cannot meet would be a lot
    // of code saying nothing. `ast::dump` overrides them, because
    // `bronze parse` runs on ONE file and must show what it parsed.
    virtual void visit(const ImportDecl&) {}
    virtual void visit(const ExportNamesDecl&) {}
    virtual void visit(const Module&) = 0;
};

}  // namespace bronze::ast
