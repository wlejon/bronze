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

struct Expr : Node {};
using ExprPtr = std::unique_ptr<Expr>;

struct NumberLit final : Expr {
    double value = 0;
    void accept(Visitor& v) const override;
};

struct StringLit final : Expr {
    std::string value;  // decoded (quotes/escapes resolved)
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

enum class UnaryOp { Not, Negate, Posate, PreInc, PreDec, PostInc, PostDec };
const char* unaryOpName(UnaryOp op);

struct Unary final : Expr {
    UnaryOp op;
    ExprPtr operand;
    void accept(Visitor& v) const override;
};

enum class BinaryOp {
    Add, Sub, Mul, Div, Mod, Less, Greater, LessEqual, GreaterEqual,
    Eq, StrictEq, Ne, StrictNe, Assign, PlusAssign, MinusAssign,
    StarAssign, SlashAssign, PercentAssign, LogicalAnd, LogicalOr, NullishCoalescing
};
const char* binaryOpName(BinaryOp op);

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

struct MemberAccess final : Expr {
    ExprPtr object;
    std::string property;
    void accept(Visitor& v) const override;
};

struct IndexAccess final : Expr {
    ExprPtr object;
    ExprPtr index;
    void accept(Visitor& v) const override;
};

struct Call final : Expr {
    ExprPtr callee;
    std::vector<ExprPtr> args;
    void accept(Visitor& v) const override;
};

struct NewExpr final : Expr {
    std::string callee;  // constructor name; only identifier callees are supported
    std::vector<ExprPtr> args;
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
    std::string key;
    ExprPtr value;
};

struct ObjectLit final : Expr {
    std::vector<ObjectProp> props;
    void accept(Visitor& v) const override;
};

struct ArrayLit final : Expr {
    std::vector<ExprPtr> elements;
    void accept(Visitor& v) const override;
};

struct Param {
    std::string name;
    std::string typeAnnotation;
};

struct FunctionExpr final : Expr {
    std::string name;
    std::vector<Param> params;
    std::string returnType;
    std::vector<StmtPtr> body;
    void accept(Visitor& v) const override;
};

struct VarDecl final : Stmt {
    bool isConst = false;
    bool isVar = false;
    std::string name;
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
    StmtPtr init;      // VarDecl or ExprStmt or empty
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

struct SwitchStmt final : Stmt {
    ExprPtr discriminant;
    void accept(Visitor& v) const override;
};

struct ForInStmt final : Stmt {
    void accept(Visitor& v) const override;
};

struct ForOfStmt final : Stmt {
    void accept(Visitor& v) const override;
};

struct TryStmt final : Stmt {
    void accept(Visitor& v) const override;
};

struct ThrowStmt final : Stmt {
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
    virtual void visit(const StringLit&) = 0;
    virtual void visit(const BoolLit&) = 0;
    virtual void visit(const NullLit&) = 0;
    virtual void visit(const UndefinedLit&) = 0;
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
    virtual void visit(const ForOfStmt&) = 0;
    virtual void visit(const TryStmt&) = 0;
    virtual void visit(const ThrowStmt&) = 0;
    virtual void visit(const FunctionDecl&) = 0;
    virtual void visit(const Module&) = 0;
};

}  // namespace bronze::ast
