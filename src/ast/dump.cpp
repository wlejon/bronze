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
    void visit(const VarDecl& n) override {
        std::string head = std::string("(") + (n.isConst ? "const " : "let ") + n.name;
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
