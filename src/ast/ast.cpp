#include "ast/ast.h"

namespace bronze::ast {

void NumberLit::accept(Visitor& v) const { v.visit(*this); }
void SpreadElement::accept(Visitor& v) const { v.visit(*this); }
void StringLit::accept(Visitor& v) const { v.visit(*this); }
void RegExpLit::accept(Visitor& v) const { v.visit(*this); }
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
void SuperCall::accept(Visitor& v) const { v.visit(*this); }
void SuperMember::accept(Visitor& v) const { v.visit(*this); }
void DestructuringAssign::accept(Visitor& v) const { v.visit(*this); }
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
void LabeledStmt::accept(Visitor& v) const { v.visit(*this); }
void TryStmt::accept(Visitor& v) const { v.visit(*this); }
void ThrowStmt::accept(Visitor& v) const { v.visit(*this); }
void ClassDecl::accept(Visitor& v) const { v.visit(*this); }
void FunctionDecl::accept(Visitor& v) const { v.visit(*this); }
void ImportDecl::accept(Visitor& v) const { v.visit(*this); }
void ExportNamesDecl::accept(Visitor& v) const { v.visit(*this); }
void Module::accept(Visitor& v) const { v.visit(*this); }

const char* unaryOpName(UnaryOp op) {
    switch (op) {
        case UnaryOp::Not: return "!";
        case UnaryOp::Negate: return "-";
        case UnaryOp::Posate: return "+";
        // The prefix and postfix forms mutate identically and EVALUATE
        // differently, so the canonical dump has to separate them: printing
        // both as "++" made `d++` and `++d` indistinguishable in the one
        // artefact the parser's tests compare, which is exactly where the
        // postfix line-break rule has to be read (docs/0014).
        case UnaryOp::PreInc: return "++pre";
        case UnaryOp::PreDec: return "--pre";
        case UnaryOp::PostInc: return "++post";
        case UnaryOp::PostDec: return "--post";
        case UnaryOp::BitNot: return "~";
        case UnaryOp::TypeOf: return "typeof";
        case UnaryOp::Void: return "void";
        case UnaryOp::Delete: return "delete";
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
        case BinaryOp::BitAnd: return "&";
        case BinaryOp::BitOr: return "|";
        case BinaryOp::BitXor: return "^";
        case BinaryOp::Shl: return "<<";
        case BinaryOp::Shr: return ">>";
        case BinaryOp::UShr: return ">>>";
        case BinaryOp::Exp: return "**";
        case BinaryOp::In: return "in";
        case BinaryOp::InstanceOf: return "instanceof";
        case BinaryOp::Comma: return ",";
        case BinaryOp::AmpAssign: return "&=";
        case BinaryOp::PipeAssign: return "|=";
        case BinaryOp::CaretAssign: return "^=";
        case BinaryOp::ShlAssign: return "<<=";
        case BinaryOp::ShrAssign: return ">>=";
        case BinaryOp::UShrAssign: return ">>>=";
        case BinaryOp::ExpAssign: return "**=";
    }
    return "?";
}

BinaryOp compoundAssignBase(BinaryOp op) {
    switch (op) {
        case BinaryOp::PlusAssign: return BinaryOp::Add;
        case BinaryOp::MinusAssign: return BinaryOp::Sub;
        case BinaryOp::StarAssign: return BinaryOp::Mul;
        case BinaryOp::SlashAssign: return BinaryOp::Div;
        case BinaryOp::PercentAssign: return BinaryOp::Mod;
        case BinaryOp::AmpAssign: return BinaryOp::BitAnd;
        case BinaryOp::PipeAssign: return BinaryOp::BitOr;
        case BinaryOp::CaretAssign: return BinaryOp::BitXor;
        case BinaryOp::ShlAssign: return BinaryOp::Shl;
        case BinaryOp::ShrAssign: return BinaryOp::Shr;
        case BinaryOp::UShrAssign: return BinaryOp::UShr;
        case BinaryOp::ExpAssign: return BinaryOp::Exp;
        default: return op;
    }
}

bool isCompoundAssignOp(BinaryOp op) { return compoundAssignBase(op) != op; }

bool isAssignOp(BinaryOp op) { return op == BinaryOp::Assign || isCompoundAssignOp(op); }

// Recursive because a pattern nests: `const [{ a }, [b]] = pairs` declares
// `a` and `b`, and a scope that allocated an environment slot for only the
// outermost level would leave an inner name with nowhere to live.
static void collectPatternNames(const BindingPattern& pattern, std::vector<std::string>& out) {
    for (const auto& elem : pattern.elements) {
        if (elem.pattern) {
            collectPatternNames(*elem.pattern, out);
        } else if (!elem.name.empty()) {
            out.push_back(elem.name);
        }
    }
}

std::vector<std::string> patternBoundNames(const BindingPattern& pattern) {
    std::vector<std::string> names;
    collectPatternNames(pattern, names);
    return names;
}

// Walks DOWN the base of a member/index/call spine looking for a `?.`, and
// stops at anything else — including a parenthesized subexpression, which is
// the one thing that ends a chain without ending the spine. `(a?.b).c` is
// therefore not an optional chain: its base is a PrimaryExpression that
// happens to be one, and `.c` on a nullish value is an ordinary property read
// (ECMA-262 13.3.9).
bool containsOptionalLink(const Expr& expr) {
    const Expr* cur = &expr;
    for (;;) {
        if (const auto* mem = dynamic_cast<const MemberAccess*>(cur)) {
            if (mem->optional) return true;
            cur = mem->object.get();
        } else if (const auto* idx = dynamic_cast<const IndexAccess*>(cur)) {
            if (idx->optional) return true;
            cur = idx->index ? idx->object.get() : nullptr;
        } else if (const auto* call = dynamic_cast<const Call*>(cur)) {
            if (call->optional) return true;
            cur = call->callee.get();
        } else {
            return false;
        }
        if (!cur || cur->parenthesized) return false;
    }
}

}  // namespace bronze::ast
