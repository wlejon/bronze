#include "ast/ast.h"

namespace bronze::ast {

void NumberLit::accept(Visitor& v) const { v.visit(*this); }
void StringLit::accept(Visitor& v) const { v.visit(*this); }
void Ident::accept(Visitor& v) const { v.visit(*this); }
void Binary::accept(Visitor& v) const { v.visit(*this); }
void MemberAccess::accept(Visitor& v) const { v.visit(*this); }
void IndexAccess::accept(Visitor& v) const { v.visit(*this); }
void Call::accept(Visitor& v) const { v.visit(*this); }
void ObjectLit::accept(Visitor& v) const { v.visit(*this); }
void ArrayLit::accept(Visitor& v) const { v.visit(*this); }
void FunctionExpr::accept(Visitor& v) const { v.visit(*this); }
void VarDecl::accept(Visitor& v) const { v.visit(*this); }
void ReturnStmt::accept(Visitor& v) const { v.visit(*this); }
void ExprStmt::accept(Visitor& v) const { v.visit(*this); }
void IfStmt::accept(Visitor& v) const { v.visit(*this); }
void FunctionDecl::accept(Visitor& v) const { v.visit(*this); }
void Module::accept(Visitor& v) const { v.visit(*this); }

const char* binaryOpName(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add: return "+";
        case BinaryOp::Sub: return "-";
        case BinaryOp::Mul: return "*";
        case BinaryOp::Div: return "/";
        case BinaryOp::Less: return "<";
        case BinaryOp::Greater: return ">";
        case BinaryOp::Eq: return "==";
        case BinaryOp::StrictEq: return "===";
        case BinaryOp::Ne: return "!=";
        case BinaryOp::StrictNe: return "!==";
        case BinaryOp::Assign: return "=";
    }
    return "?";
}

}  // namespace bronze::ast
