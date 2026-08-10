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

enum class BinaryOp { Add, Sub, Mul, Div, Less, Greater, Eq, StrictEq, Ne, StrictNe };
const char* binaryOpName(BinaryOp op);

struct Binary final : Expr {
    BinaryOp op;
    ExprPtr lhs;
    ExprPtr rhs;
    void accept(Visitor& v) const override;
};

struct Call final : Expr {
    ExprPtr callee;
    std::vector<ExprPtr> args;
    void accept(Visitor& v) const override;
};

// ---- Statements / declarations ---------------------------------------------

struct Stmt : Node {};
using StmtPtr = std::unique_ptr<Stmt>;

struct VarDecl final : Stmt {
    bool isConst = false;
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

struct Param {
    std::string name;
    std::string typeAnnotation;
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
    virtual void visit(const Ident&) = 0;
    virtual void visit(const Binary&) = 0;
    virtual void visit(const Call&) = 0;
    virtual void visit(const VarDecl&) = 0;
    virtual void visit(const ReturnStmt&) = 0;
    virtual void visit(const ExprStmt&) = 0;
    virtual void visit(const IfStmt&) = 0;
    virtual void visit(const FunctionDecl&) = 0;
    virtual void visit(const Module&) = 0;
};

}  // namespace bronze::ast
