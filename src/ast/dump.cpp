#include "ast/dump.h"

#include <sstream>

namespace bronze::ast {
namespace {

class DumpVisitor final : public Visitor {
public:
    std::string result;

    void visit(const NumberLit& n) override {
        std::ostringstream os;
        os << n.value;
        emit("(number " + os.str() + ")");
    }
    void visit(const StringLit& n) override { emit("(string \"" + n.value + "\")"); }
    void visit(const Ident& n) override { emit("(ident " + n.name + ")"); }
    void visit(const Binary& n) override {
        emit(std::string("(binary ") + binaryOpName(n.op));
        indented([&] {
            n.lhs->accept(*this);
            n.rhs->accept(*this);
        });
        emit(")");
    }
    void visit(const MemberAccess& n) override {
        emit("(member ." + n.property);
        indented([&] { n.object->accept(*this); });
        emit(")");
    }
    void visit(const IndexAccess& n) override {
        emit("(index");
        indented([&] {
            n.object->accept(*this);
            n.index->accept(*this);
        });
        emit(")");
    }
    void visit(const Call& n) override {
        emit("(call");
        indented([&] {
            n.callee->accept(*this);
            for (const auto& a : n.args) a->accept(*this);
        });
        emit(")");
    }
    void visit(const ObjectLit& n) override {
        emit("(object");
        indented([&] {
            for (const auto& p : n.props) {
                emit("(prop " + p.key);
                indented([&] { p.value->accept(*this); });
                emit(")");
            }
        });
        emit(")");
    }
    void visit(const ArrayLit& n) override {
        emit("(array");
        indented([&] {
            for (const auto& e : n.elements) e->accept(*this);
        });
        emit(")");
    }
    void visit(const FunctionExpr& n) override {
        std::string head = "(function-expr " + (n.name.empty() ? "<anon>" : n.name) + " (";
        for (size_t i = 0; i < n.params.size(); ++i) {
            if (i > 0) head += ' ';
            head += n.params[i].name;
            if (!n.params[i].typeAnnotation.empty()) head += ": " + n.params[i].typeAnnotation;
        }
        head += ')';
        if (!n.returnType.empty()) head += ": " + n.returnType;
        emit(head);
        indented([this, &n] {
            for (const auto& s : n.body) s->accept(*this);
        });
        emit(")");
    }
    void visit(const BoolLit& n) override { emit(std::string("(bool ") + (n.value ? "true" : "false") + ")"); }
    void visit(const NullLit&) override { emit("(null)"); }
    void visit(const UndefinedLit&) override { emit("(undefined)"); }
    void visit(const Unary& n) override {
        emit(std::string("(unary ") + unaryOpName(n.op));
        indented([&] { n.operand->accept(*this); });
        emit(")");
    }
    void visit(const Ternary& n) override {
        emit("(ternary");
        indented([&] {
            n.condition->accept(*this);
            n.thenExpr->accept(*this);
            n.elseExpr->accept(*this);
        });
        emit(")");
    }
    void visit(const BlockStmt& n) override {
        emit("(block");
        indented([&] { for (const auto& s : n.stmts) s->accept(*this); });
        emit(")");
    }
    void visit(const VarDecl& n) override {
        std::string head = std::string("(") + (n.isConst ? "const " : n.isVar ? "var " : "let ") + n.name;
        if (!n.typeAnnotation.empty()) head += ": " + n.typeAnnotation;
        emit(head);
        if (n.init) {
            indented([&] { n.init->accept(*this); });
        }
        emit(")");
    }
    void visit(const ReturnStmt& n) override {
        emit("(return");
        if (n.value) indented([&] { n.value->accept(*this); });
        emit(")");
    }
    void visit(const ExprStmt& n) override {
        emit("(expr");
        indented([&] { n.expr->accept(*this); });
        emit(")");
    }
    void visit(const IfStmt& n) override {
        emit("(if");
        indented([&] {
            n.condition->accept(*this);
            emit("(then");
            indented([&] {
                for (const auto& s : n.thenBody) s->accept(*this);
            });
            emit(")");
            if (!n.elseBody.empty()) {
                emit("(else");
                indented([&] {
                    for (const auto& s : n.elseBody) s->accept(*this);
                });
                emit(")");
            }
        });
        emit(")");
    }
    void visit(const WhileStmt& n) override {
        emit("(while");
        indented([&] {
            n.condition->accept(*this);
            for (const auto& s : n.body) s->accept(*this);
        });
        emit(")");
    }
    void visit(const DoWhileStmt& n) override {
        emit("(do-while");
        indented([&] {
            for (const auto& s : n.body) s->accept(*this);
            n.condition->accept(*this);
        });
        emit(")");
    }
    void visit(const ForStmt& n) override {
        emit("(for");
        indented([&] {
            if (n.init) n.init->accept(*this);
            if (n.condition) n.condition->accept(*this);
            if (n.update) n.update->accept(*this);
            for (const auto& s : n.body) s->accept(*this);
        });
        emit(")");
    }
    void visit(const BreakStmt& n) override { emit("(break" + (n.label.empty() ? "" : " " + n.label) + ")"); }
    void visit(const ContinueStmt& n) override { emit("(continue" + (n.label.empty() ? "" : " " + n.label) + ")"); }
    void visit(const SwitchStmt&) override { emit("(switch)"); }
    void visit(const ForInStmt&) override { emit("(for-in)"); }
    void visit(const ForOfStmt&) override { emit("(for-of)"); }
    void visit(const TryStmt&) override { emit("(try)"); }
    void visit(const ThrowStmt&) override { emit("(throw)"); }
    void visit(const FunctionDecl& n) override {
        std::string head = "(function " + n.name + " (";
        for (size_t i = 0; i < n.params.size(); ++i) {
            if (i > 0) head += ' ';
            head += n.params[i].name;
            if (!n.params[i].typeAnnotation.empty()) head += ": " + n.params[i].typeAnnotation;
        }
        head += ')';
        if (!n.returnType.empty()) head += ": " + n.returnType;
        if (n.isExported) head += " exported";
        emit(head);
        indented([&] {
            for (const auto& s : n.body) s->accept(*this);
        });
        emit(")");
    }
    void visit(const Module& n) override {
        emit("(module " + n.name);
        indented([&] {
            for (const auto& s : n.body) s->accept(*this);
        });
        emit(")");
    }

private:
    int depth_ = 0;

    void emit(const std::string& line) {
        result.append(static_cast<size_t>(depth_) * 2, ' ');
        result += line;
        result += '\n';
    }
    template <typename F>
    void indented(F f) {
        ++depth_;
        f();
        --depth_;
    }
};

}  // namespace

std::string dump(const Module& module) {
    DumpVisitor v;
    module.accept(v);
    return v.result;
}

}  // namespace bronze::ast
