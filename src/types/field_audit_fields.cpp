// A field declared without an initializer, and what the write audit makes of
// its `undefined`.
//
// `x;` (or TypeScript's `x: number;`) defines the property as `undefined` when
// the instance is initialized, and a constructor that assigns `this.x = ...`
// moments later is the common way to write a class. Recorded as a write of
// `undefined`, the declaration would refuse every Number claim on the name
// program-wide, for a value no read can see — which is what deleting the
// declaration in the parser used to buy, at the price of changing the
// language (the key's order, the shadowing of an inherited accessor, what
// `Object.keys` sees mid-constructor).
//
// So the audit asks the question the deletion answered by fiat: can anything
// READ the property between its definition and the constructor's assignment?
// The window is every later field initializer, then the constructor's
// statements up to that assignment, and the assignment's own right-hand side.
// It is unobservable when nothing in it can reach the object:
//
//   - in a BASE class the instance is fresh: no code holds it but `this`, so
//     the window must not mention `this` (an arrow that captures it counts,
//     since it could be called), and every statement before the assignment
//     must be a write to one of the class's own declared fields or to a
//     private one. A declared field is an own data property by then, so the
//     write cannot reach a setter — this class's, a base's or a subclass's —
//     that would hand `this` to code.
//   - in a DERIVED class the base constructor has already run and may have
//     stored `this` anywhere, so the window may run no code at all: every
//     expression in it must be a literal, a binding, a function created and
//     not called, or an array or object literal of those.
//
// A field the window does not prove keeps its `undefined` write, which
// refuses the name exactly as before.

#include <string>
#include <vector>

#include "types/field_audit.h"
#include "types/walk.h"

namespace bronze::types {

namespace {

// Whether an expression mentions the receiver, the way `ast::usesThis` asks of
// a body: an arrow's body counts, another function's or class's does not.
class ThisMention final : public Walker {
public:
    bool found = false;
    void visit(const ast::ThisExpr&) override { found = true; }
    void visit(const ast::SuperCall&) override { found = true; }
    void visit(const ast::SuperMember&) override { found = true; }
    void visit(const ast::FunctionExpr& n) override {
        if (n.isArrow) Walker::visit(n);
    }
    void visit(const ast::FunctionDecl&) override {}
    void visit(const ast::ClassDecl&) override {}
    void visit(const ast::ClassExpr&) override {}
};

bool mentionsThis(const ast::Expr& e) {
    ThisMention v;
    e.accept(v);
    return v.found;
}

// An expression whose evaluation runs no code but its own. Narrow on purpose:
// every operator that can reach ToPrimitive, every read that can reach a
// getter and every call is out.
bool runsNoCode(const ast::Expr& e) {
    if (dynamic_cast<const ast::NumberLit*>(&e) || dynamic_cast<const ast::StringLit*>(&e) ||
        dynamic_cast<const ast::BoolLit*>(&e) || dynamic_cast<const ast::NullLit*>(&e) ||
        dynamic_cast<const ast::UndefinedLit*>(&e) || dynamic_cast<const ast::BigIntLit*>(&e) ||
        dynamic_cast<const ast::Ident*>(&e)) {
        return true;
    }
    if (const auto* f = dynamic_cast<const ast::FunctionExpr*>(&e)) {
        return !f->isArrow || !mentionsThis(e);
    }
    if (const auto* a = dynamic_cast<const ast::ArrayLit*>(&e)) {
        for (const auto& el : a->elements) {
            if (el && !runsNoCode(*el)) return false;
        }
        return true;
    }
    if (const auto* o = dynamic_cast<const ast::ObjectLit*>(&e)) {
        for (const auto& p : o->props) {
            // A spread's value is the SpreadElement, which reads its source's
            // properties and is refused below like any other node.
            if (p.keyExpr || p.accessor != ast::AccessorKind::None) return false;
            if (p.value && !runsNoCode(*p.value)) return false;
        }
        return true;
    }
    return false;
}

bool quiet(const ast::Expr& e, bool derived) {
    return derived ? runsNoCode(e) : !mentionsThis(e);
}

bool declaresField(const std::vector<ast::ClassMethod>& methods, const std::string& name) {
    for (const auto& m : methods) {
        if (m.isField && !m.isStatic && !m.computed() && m.name == name) return true;
    }
    return false;
}

const ast::MemberAccess* thisMemberTarget(const ast::Stmt& s, const ast::Expr** rhs) {
    const auto* es = dynamic_cast<const ast::ExprStmt*>(&s);
    if (es == nullptr) return nullptr;
    const auto* bin = dynamic_cast<const ast::Binary*>(es->expr.get());
    if (bin == nullptr || bin->op != ast::BinaryOp::Assign) return nullptr;
    const auto* mem = dynamic_cast<const ast::MemberAccess*>(bin->lhs.get());
    if (mem == nullptr || dynamic_cast<const ast::ThisExpr*>(mem->object.get()) == nullptr) {
        return nullptr;
    }
    *rhs = bin->rhs.get();
    return mem;
}

}  // namespace

bool fieldUndefinedUnobserved(const std::vector<ast::ClassMethod>& methods, size_t fieldIndex,
                              bool derived) {
    if (fieldIndex >= methods.size()) return false;
    const std::string& name = methods[fieldIndex].name;

    // The later fields' initializers run first, in definition order.
    for (size_t i = fieldIndex + 1; i < methods.size(); ++i) {
        const ast::ClassMethod& m = methods[i];
        if (!m.isField || m.isStatic || !m.init) continue;
        if (!quiet(*m.init, derived)) return false;
    }

    const ast::ClassMethod* ctor = nullptr;
    for (const auto& m : methods) {
        if (m.isConstructor && m.fn) ctor = &m;
    }
    if (ctor == nullptr) return false;
    const auto& body = ctor->fn->body;
    size_t i = 0;
    if (derived) {
        // The fields are defined when `super()` returns; a constructor whose
        // first call is not a statement of the body proper is not read here.
        while (i < body.size()) {
            const auto* es = dynamic_cast<const ast::ExprStmt*>(body[i].get());
            ++i;
            if (es != nullptr && dynamic_cast<const ast::SuperCall*>(es->expr.get())) break;
            if (i == body.size()) return false;
        }
    } else {
        // A base class's parameter defaults run after its fields are defined
        // (10.2.1.3: the fields at [[Construct]], the defaults in the body's
        // instantiation), so they are in the window too.
        for (const auto& p : ctor->fn->params) {
            if (p.defaultValue && mentionsThis(*p.defaultValue)) return false;
            if (p.pattern) return false;
        }
    }
    for (; i < body.size(); ++i) {
        const ast::Expr* rhs = nullptr;
        const ast::MemberAccess* target = thisMemberTarget(*body[i], &rhs);
        if (target == nullptr || rhs == nullptr) return false;
        if (!quiet(*rhs, derived)) return false;
        if (!target->isPrivate && target->property == name) return true;
        if (!target->isPrivate && !declaresField(methods, target->property)) return false;
    }
    return false;
}

}  // namespace bronze::types
