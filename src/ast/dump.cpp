#include "ast/dump.h"

#include <charconv>
#include <string>

namespace bronze::ast {
namespace {

// The shortest text that round-trips to this double, which is what the IL
// printer already uses. `ostringstream <<` was neither: its default precision
// is six significant digits, so `1000000` dumped as `1e+06` and `123.4567`
// as `123.457` — two different literals could dump identically, in the one
// artefact the parser's tests compare. It also reads the stream's locale for
// the decimal point, which the determinism rule (docs/0001) forbids outright
// on an output path, and `bronze parse` prints this to stdout.
std::string formatNumber(double v) {
    char buf[32];
    const auto res = std::to_chars(buf, buf + sizeof(buf), v);
    return std::string(buf, res.ptr);
}

class DumpVisitor final : public Visitor {
public:
    std::string result;

    void visit(const NumberLit& n) override {
        emit("(number " + formatNumber(n.value) + ")");
    }
    void visit(const StringLit& n) override { emit("(string \"" + n.value + "\")"); }
    void visit(const RegExpLit& n) override {
        emit("(regexp /" + n.pattern + "/" + n.flags + ")");
    }
    void visit(const Ident& n) override { emit("(ident " + n.name + ")"); }
    void visit(const Binary& n) override {
        emit(std::string("(binary ") + binaryOpName(n.op));
        indented([&] {
            n.lhs->accept(*this);
            n.rhs->accept(*this);
        });
        emit(")");
    }
    // An optional link prints under its own head. `a.b` and `a?.b` differ in
    // what they evaluate, not only in what they produce, so two constructs
    // that lower differently must not dump identically.
    void visit(const MemberAccess& n) override {
        emit("(member " + std::string(n.optional ? "?." : ".") + n.property);
        indented([&] { n.object->accept(*this); });
        emit(")");
    }
    void visit(const IndexAccess& n) override {
        emit(n.optional ? "(index?." : "(index");
        indented([&] {
            n.object->accept(*this);
            n.index->accept(*this);
        });
        emit(")");
    }
    void visit(const Call& n) override {
        emit(n.optional ? "(call?." : "(call");
        indented([&] {
            n.callee->accept(*this);
            for (const auto& a : n.args) a->accept(*this);
        });
        emit(")");
    }
    void visit(const NewExpr& n) override {
        // A bare NAME prints on the head line, the way a member access prints
        // its property: it is the form whose constructor identity inference
        // can still recover (docs/0025 decision 3), and seeing that at a
        // glance is the point of the dump. Any other callee prints as the
        // first child, so a mis-grouped `new a.b().c` cannot look like a
        // correctly grouped `new a.b.c()`.
        const auto* ident = dynamic_cast<const Ident*>(n.callee.get());
        emit(ident != nullptr ? "(new " + ident->name : "(new");
        indented([&] {
            if (ident == nullptr) n.callee->accept(*this);
            for (const auto& a : n.args) a->accept(*this);
        });
        emit(")");
    }
    void visit(const ObjectLit& n) override {
        emit("(object");
        indented([&] {
            for (const auto& p : n.props) {
                // A computed key is a runtime ToPropertyKey of an evaluated
                // expression and a written one is a compile-time constant, so
                // the two must not dump the same (docs/0012 decision 3).
                // A CoverInitializedName is only ever a pattern; it dumps
                // under its own head so it is not mistaken for the object
                // literal `{ x: x = 1 }`, which is a legal program that
                // means something else entirely.
                const bool spread =
                    dynamic_cast<const SpreadElement*>(p.value.get()) != nullptr;
                // An accessor half dumps under its own head: `get x` and
                // `set x` build ONE property with two halves, and a form
                // that printed both as `(prop x` would make the two
                // indistinguishable from an ordinary property written twice.
                emit(spread                ? std::string("(prop-spread")
                     : p.coverInitialized  ? "(prop-cover-init " + p.key
                     : p.computed()        ? std::string("(prop-computed")
                     : p.accessor == AccessorKind::Getter ? "(prop-get " + p.key
                     : p.accessor == AccessorKind::Setter ? "(prop-set " + p.key
                                           : "(prop " + p.key);
                indented([&] {
                    if (p.keyExpr) p.keyExpr->accept(*this);
                    p.value->accept(*this);
                });
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
    // A spread prints under its own head. It contributes several elements
    // where every neighbour contributes one, so `[a]` and `[...a]` must not
    // dump the same.
    void visit(const SpreadElement& n) override {
        emit("(spread");
        indented([&] { n.argument->accept(*this); });
        emit(")");
    }
    void visit(const DestructuringAssign& n) override {
        emit("(destructuring-assign");
        indented([&] {
            dumpPattern(n.pattern.get());
            n.value->accept(*this);
        });
        emit(")");
    }
    void visit(const FunctionExpr& n) override {
        // An arrow prints under its own head: it is not a shorter spelling
        // of a function expression, it has no `this` of its own, and two
        // constructs that lower differently must not dump identically.
        emit(std::string(n.isArrow ? "(arrow-expr " : "(function-expr ") +
             (n.name.empty() ? "<anon>" : n.name) + paramHead(n.params) +
             (n.returnType.empty() ? "" : ": " + n.returnType));
        indented([this, &n] {
            dumpParamDetails(n.params);
            for (const auto& s : n.body) s->accept(*this);
        });
        emit(")");
    }
    void visit(const SuperCall& n) override {
        emit("(super-call " + n.baseName);
        indented([&] {
            for (const auto& a : n.args) a->accept(*this);
        });
        emit(")");
    }
    void visit(const SuperMember& n) override {
        emit("(super-member " + n.baseName + "." + n.property + ")");
    }
    void visit(const ClassDecl& n) override {
        emit("(class " + n.name + (n.superName.empty() ? "" : " extends " + n.superName));
        indented([&] {
            for (const auto& m : n.methods) {
                const char* head = m.accessor == AccessorKind::Getter
                                       ? (m.isStatic ? "(static-get " : "(get ")
                                   : m.accessor == AccessorKind::Setter
                                       ? (m.isStatic ? "(static-set " : "(set ")
                                       : (m.isStatic ? "(static-method " : "(method ");
                emit(std::string(head) + m.name);
                indented([&] { m.fn->accept(*this); });
                emit(")");
            }
        });
        emit(")");
    }
    void visit(const BoolLit& n) override { emit(std::string("(bool ") + (n.value ? "true" : "false") + ")"); }
    void visit(const NullLit&) override { emit("(null)"); }
    void visit(const UndefinedLit&) override { emit("(undefined)"); }
    void visit(const ThisExpr&) override { emit("(this)"); }
    void visit(const Unary& n) override {
        emit(std::string("(unary ") + unaryOpName(n.op));
        indented([&] { n.operand->accept(*this); });
        emit(")");
    }
    void visit(const TemplateLit& n) override {
        emit("(template");
        indented([&] {
            for (size_t i = 0; i < n.quasis.size(); ++i) {
                emit("(quasi \"" + n.quasis[i] + "\")");
                if (i < n.exprs.size()) n.exprs[i]->accept(*this);
            }
        });
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
        std::string head = std::string("(") + (n.isConst ? "const " : n.isVar ? "var " : "let ") +
                           (n.pattern ? std::string("<pattern>") : n.name);
        if (!n.typeAnnotation.empty()) head += ": " + n.typeAnnotation;
        emit(head);
        indented([&] {
            if (n.pattern) dumpPattern(n.pattern.get());
            if (n.init) n.init->accept(*this);
        });
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
            for (const auto& s : n.init) s->accept(*this);
            if (n.condition) n.condition->accept(*this);
            if (n.update) n.update->accept(*this);
            for (const auto& s : n.body) s->accept(*this);
        });
        emit(")");
    }
    void visit(const BreakStmt& n) override { emit("(break" + (n.label.empty() ? "" : " " + n.label) + ")"); }
    void visit(const ContinueStmt& n) override { emit("(continue" + (n.label.empty() ? "" : " " + n.label) + ")"); }
    void visit(const SwitchStmt& n) override {
        emit("(switch");
        indented([&] {
            if (n.discriminant) n.discriminant->accept(*this);
            for (const auto& c : n.cases) {
                // `default` prints under a different head from `case`: where
                // the default sits decides what falls into and out of it, so
                // a dump that spelled them alike would hide the one thing
                // about a case list that is not obvious from source order.
                emit(c.test ? "(case" : "(default");
                indented([&] {
                    if (c.test) c.test->accept(*this);
                    for (const auto& s : c.body) s->accept(*this);
                });
                emit(")");
            }
        });
        emit(")");
    }
    void visit(const ForInStmt& n) override {
        emit("(for-in " + (n.pattern ? std::string("<pattern>") : n.name));
        indented([&] {
            if (n.pattern) dumpPattern(n.pattern.get());
            if (n.object) n.object->accept(*this);
            for (const auto& s : n.body) s->accept(*this);
        });
        emit(")");
    }
    void visit(const LabeledStmt& n) override {
        emit("(label " + n.label);
        indented([&] {
            if (n.body) n.body->accept(*this);
        });
        emit(")");
    }
    void visit(const ForOfStmt& n) override {
        emit("(for-of " + (n.pattern ? std::string("<pattern>") : n.name));
        indented([&] {
            if (n.pattern) dumpPattern(n.pattern.get());
            if (n.iterable) n.iterable->accept(*this);
            for (const auto& s : n.body) s->accept(*this);
        });
        emit(")");
    }
    void visit(const TryStmt& n) override {
        emit("(try");
        indented([&] {
            for (const auto& s : n.body) s->accept(*this);
            if (n.hasCatch) {
                emit("(catch " + (n.catchPattern    ? std::string("<pattern>")
                                  : n.hasCatchParam ? n.catchName
                                                    : std::string("<none>")));
                indented([&] {
                    if (n.catchPattern) dumpPattern(n.catchPattern.get());
                    for (const auto& s : n.catchBody) s->accept(*this);
                });
                emit(")");
            }
            if (n.hasFinally) {
                emit("(finally");
                indented([&] {
                    for (const auto& s : n.finallyBody) s->accept(*this);
                });
                emit(")");
            }
        });
        emit(")");
    }
    void visit(const ThrowStmt& n) override {
        emit("(throw");
        indented([&] {
            if (n.value) n.value->accept(*this);
        });
        emit(")");
    }
    void visit(const FunctionDecl& n) override {
        std::string head = "(function " + n.name + paramHead(n.params);
        if (!n.returnType.empty()) head += ": " + n.returnType;
        if (n.isExported) head += " exported";
        emit(head);
        indented([&] {
            dumpParamDetails(n.params);
            for (const auto& s : n.body) s->accept(*this);
        });
        emit(")");
    }
    // `bronze parse` runs on one file, so it shows the import and export
    // nodes the linker would later erase. Each binding form dumps under its
    // own head: a default, a namespace and a named import bind by three
    // different rules and two of them dumping identically would hide which
    // one the parser chose.
    void visit(const ImportDecl& n) override {
        emit("(import \"" + n.specifier + "\"");
        indented([&] {
            for (const auto& s : n.specifiers) {
                if (s.isNamespace) {
                    emit("(namespace " + s.local + ")");
                } else if (s.isDefault) {
                    emit("(default " + s.local + ")");
                } else {
                    emit("(named " + s.imported + " as " + s.local + ")");
                }
            }
        });
        emit(")");
    }
    void visit(const ExportNamesDecl& n) override {
        std::string head = "(export";
        if (n.isStar) head += " *";
        if (!n.starAlias.empty()) head += " as " + n.starAlias;
        if (n.hasFrom) head += " from \"" + n.fromSpecifier + "\"";
        emit(head);
        indented([&] {
            for (const auto& s : n.specifiers) emit("(name " + s.local + " as " + s.exported + ")");
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

    // The parameter list as it appears in the head. Every form a parameter
    // can take is marked, because they lower differently: `...` for a rest
    // parameter, a trailing `=` for one with a default (whose expression is
    // dumped below, where an expression can be), and `<pattern>` for a
    // destructuring parameter.
    static std::string paramHead(const std::vector<Param>& params) {
        std::string head = " (";
        for (size_t i = 0; i < params.size(); ++i) {
            if (i > 0) head += ' ';
            if (params[i].isRest) head += "...";
            head += params[i].pattern ? std::string("<pattern>") : params[i].name;
            if (!params[i].typeAnnotation.empty()) head += ": " + params[i].typeAnnotation;
            if (params[i].defaultValue) head += " =";
        }
        return head + ')';
    }

    void dumpParamDetails(const std::vector<Param>& params) {
        for (const auto& p : params) {
            if (!p.pattern && !p.defaultValue) continue;
            emit("(param " + (p.pattern ? std::string("<pattern>") : p.name));
            indented([&] {
                if (p.pattern) dumpPattern(p.pattern.get());
                if (p.defaultValue) p.defaultValue->accept(*this);
            });
            emit(")");
        }
    }

    void dumpPattern(const BindingPattern* pattern) {
        if (!pattern) return;
        emit(pattern->isObject ? "(pattern-object" : "(pattern-array");
        indented([&] {
            for (const auto& elem : pattern->elements) {
                std::string head = elem.isRest ? "(elem ..." : "(elem ";
                if (!elem.key.empty()) head += elem.key + ": ";
                if (elem.keyExpr) head += "[computed]: ";
                head += elem.pattern ? std::string("<pattern>") : elem.name;
                emit(head);
                indented([&] {
                    if (elem.keyExpr) elem.keyExpr->accept(*this);
                    if (elem.pattern) dumpPattern(elem.pattern.get());
                    if (elem.defaultValue) {
                        emit("(default");
                        indented([&] { elem.defaultValue->accept(*this); });
                        emit(")");
                    }
                });
                emit(")");
            }
        });
        emit(")");
    }

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
