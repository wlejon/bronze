#include "ast/ast.h"

namespace bronze::ast {

void NumberLit::accept(Visitor& v) const { v.visit(*this); }
void StringLit::accept(Visitor& v) const { v.visit(*this); }
void BoolLit::accept(Visitor& v) const { v.visit(*this); }
void NullLit::accept(Visitor& v) const { v.visit(*this); }
void UndefinedLit::accept(Visitor& v) const { v.visit(*this); }
void ThisExpr::accept(Visitor& v) const { v.visit(*this); }
void Ident::accept(Visitor& v) const { v.visit(*this); }
void Unary::accept(Visitor& v) const { v.visit(*this); }
void Binary::accept(Visitor& v) const { v.visit(*this); }
void Ternary::accept(Visitor& v) const { v.visit(*this); }
void TemplateLit::accept(Visitor& v) const { v.visit(*this); }
void MemberAccess::accept(Visitor& v) const { v.visit(*this); }
void IndexAccess::accept(Visitor& v) const { v.visit(*this); }
void Call::accept(Visitor& v) const { v.visit(*this); }
void NewExpr::accept(Visitor& v) const { v.visit(*this); }
void ObjectLit::accept(Visitor& v) const { v.visit(*this); }
void ArrayLit::accept(Visitor& v) const { v.visit(*this); }
void FunctionExpr::accept(Visitor& v) const { v.visit(*this); }
void BlockStmt::accept(Visitor& v) const { v.visit(*this); }
void VarDecl::accept(Visitor& v) const { v.visit(*this); }
void ReturnStmt::accept(Visitor& v) const { v.visit(*this); }
void ExprStmt::accept(Visitor& v) const { v.visit(*this); }
void IfStmt::accept(Visitor& v) const { v.visit(*this); }
void WhileStmt::accept(Visitor& v) const { v.visit(*this); }
void DoWhileStmt::accept(Visitor& v) const { v.visit(*this); }
void ForStmt::accept(Visitor& v) const { v.visit(*this); }
void BreakStmt::accept(Visitor& v) const { v.visit(*this); }
void ContinueStmt::accept(Visitor& v) const { v.visit(*this); }
void SwitchStmt::accept(Visitor& v) const { v.visit(*this); }
void ForInStmt::accept(Visitor& v) const { v.visit(*this); }
void ForOfStmt::accept(Visitor& v) const { v.visit(*this); }
void TryStmt::accept(Visitor& v) const { v.visit(*this); }
void ThrowStmt::accept(Visitor& v) const { v.visit(*this); }
void FunctionDecl::accept(Visitor& v) const { v.visit(*this); }
void Module::accept(Visitor& v) const { v.visit(*this); }

const char* unaryOpName(UnaryOp op) {
    switch (op) {
        case UnaryOp::Not: return "!";
        case UnaryOp::Negate: return "-";
        case UnaryOp::Posate: return "+";
        case UnaryOp::PreInc: return "++";
        case UnaryOp::PreDec: return "--";
        case UnaryOp::PostInc: return "++";
        case UnaryOp::PostDec: return "--";
    }
    return "?";
}

const char* binaryOpName(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add: return "+";
        case BinaryOp::Sub: return "-";
        case BinaryOp::Mul: return "*";
        case BinaryOp::Div: return "/";
        case BinaryOp::Mod: return "%";
        case BinaryOp::Less: return "<";
        case BinaryOp::Greater: return ">";
        case BinaryOp::LessEqual: return "<=";
        case BinaryOp::GreaterEqual: return ">=";
        case BinaryOp::Eq: return "==";
        case BinaryOp::StrictEq: return "===";
        case BinaryOp::Ne: return "!=";
        case BinaryOp::StrictNe: return "!==";
        case BinaryOp::Assign: return "=";
        case BinaryOp::PlusAssign: return "+=";
        case BinaryOp::MinusAssign: return "-=";
        case BinaryOp::StarAssign: return "*=";
        case BinaryOp::SlashAssign: return "/=";
        case BinaryOp::PercentAssign: return "%=";
        case BinaryOp::LogicalAnd: return "&&";
        case BinaryOp::LogicalOr: return "||";
        case BinaryOp::NullishCoalescing: return "??";
    }
    return "?";
}

}  // namespace bronze::ast
