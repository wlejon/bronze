// The field-type half of `ClassLayoutTable`: not which slots an instance has,
// which is `class_layout.cpp`'s question, but what those slots HOLD.
//
// The two are separate files for the reason they were already separate passes.
// A layout is decided once, from the shape of the construction sequence, and
// never moves. A field's TYPE moves: `this.x = x` says nothing until the
// call-graph fixpoint has typed `x`, which it does by joining over every
// `new C(...)` in the program (types/ctor_ident.h) — so this harvest runs again
// on every round of that fixpoint, and its answer only widens.

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "types/class_layout.h"
#include "types/walk.h"

namespace bronze::types {
namespace {

// What one `this.<field> = <rhs>` says about the field's type.
//
// Syntactic, one level deep, and deliberately so: the flow analysis that could
// answer this properly runs AFTER this table is built (it consumes it), so
// asking it here would be a cycle. The forms recognised are the ones that carry
// a class identity or a number — which is the whole of what a later fixed-slot
// or unboxed-slot decision can use — and everything else answers `Dynamic`,
// which poisons the join and is always the safe answer.
Type harvestFieldType(const ast::Expr& rhs, const std::map<std::string, size_t>& classByName,
                      const std::vector<ClassLayout>& classes,
                      const std::map<std::string, Type>* ctorParams) {
    // A bare parameter of the constructor now being walked. The one form whose
    // answer is not syntactic: the call-graph fixpoint joined it over every
    // construction site the program contains (types/ctor_ident.h), and it is
    // handed here only for a name the constructor body never rebinds or writes.
    if (ctorParams != nullptr) {
        if (const auto* id = dynamic_cast<const ast::Ident*>(&rhs)) {
            const auto it = ctorParams->find(id->name);
            if (it != ctorParams->end()) return it->second;
        }
    }
    if (dynamic_cast<const ast::NumberLit*>(&rhs)) return Type::number();
    if (dynamic_cast<const ast::StringLit*>(&rhs)) return Type::string();
    if (dynamic_cast<const ast::BoolLit*>(&rhs)) return Type::boolean();
    if (dynamic_cast<const ast::NullLit*>(&rhs)) return Type::null();
    if (dynamic_cast<const ast::UndefinedLit*>(&rhs)) return Type::undefined();
    if (const auto* u = dynamic_cast<const ast::Unary*>(&rhs)) {
        // `-1` is a unary minus over a literal, which is how every negative
        // default in three.js is written.
        if (u->op == ast::UnaryOp::Negate || u->op == ast::UnaryOp::Posate) {
            if (dynamic_cast<const ast::NumberLit*>(u->operand.get())) return Type::number();
        }
        return Type::dynamic();
    }
    if (const auto* n = dynamic_cast<const ast::NewExpr*>(&rhs)) {
        const auto* id = dynamic_cast<const ast::Ident*>(n->callee.get());
        if (id == nullptr) return Type::dynamic();
        const auto it = classByName.find(id->name);
        if (it == classByName.end()) return Type::dynamic();
        const ShapeClassId cls = classes[it->second].shapeClass;
        return cls == kNoShapeClass ? Type::dynamic() : Type::object(cls);
    }
    return Type::dynamic();
}

// Joins field types from every `this.<name> = <rhs>` in one method body.
class FieldTypeWalker final : public Walker {
public:
    FieldTypeWalker(std::map<std::string, Type>& out,
                    const std::map<std::string, size_t>& classByName,
                    const std::vector<ClassLayout>& classes,
                    const std::map<std::string, Type>* ctorParams = nullptr)
        : out_(out), classByName_(classByName), classes_(classes), ctorParams_(ctorParams) {}

    void visit(const ast::FunctionExpr& n) override {
        if (n.isArrow) Walker::visit(n);
    }
    void visit(const ast::FunctionDecl&) override {}
    void visit(const ast::ClassDecl&) override {}
    void visit(const ast::ClassExpr&) override {}

    void visit(const ast::Binary& n) override {
        if (n.op == ast::BinaryOp::Assign) {
            if (const auto* m = dynamic_cast<const ast::MemberAccess*>(n.lhs.get())) {
                if (dynamic_cast<const ast::ThisExpr*>(m->object.get()) && !m->isPrivate) {
                    record(m->property,
                           harvestFieldType(*n.rhs, classByName_, classes_, ctorParams_));
                }
            }
        } else if (ast::isAssignOp(n.op)) {
            // A compound assignment's result is an operator's, not the RHS's.
            // Nothing here models operators, so poison the field.
            if (const auto* m = dynamic_cast<const ast::MemberAccess*>(n.lhs.get())) {
                if (dynamic_cast<const ast::ThisExpr*>(m->object.get())) {
                    record(m->property, Type::dynamic());
                }
            }
        }
        Walker::visit(n);
    }

private:
    void record(const std::string& name, Type t) {
        const auto it = out_.find(name);
        out_[name] = it == out_.end() ? t : join(it->second, t);
    }
    std::map<std::string, Type>& out_;
    const std::map<std::string, size_t>& classByName_;
    const std::vector<ClassLayout>& classes_;
    // Non-null only while the CONSTRUCTOR's body is being walked: a method's
    // parameter of the same name is a different binding entirely.
    const std::map<std::string, Type>* ctorParams_ = nullptr;
};

}  // namespace

// The field-type harvest, for every class at once.
//
// `byClass` answers a bare identifier in `this.<f> = <name>` when the name is a
// parameter of THAT class's constructor: the one thing about a class body this
// pass cannot read off the syntax, and the one that decides whether three.js's
// math classes carry a number. Null runs the purely syntactic harvest, which is
// what `build` does before any parameter type exists.
//
// Recomputed from scratch rather than joined into, because the answer has to be
// able to IMPROVE: `this.x = x` harvested `dynamic` from an untyped parameter,
// and a join with the better answer would keep the worse one forever.
void ClassLayoutTable::harvestFieldTypes(
    const std::map<std::string, std::map<std::string, Type>>* byClass) {
    for (size_t i = 0; i < classes_.size(); ++i) {
        const std::vector<ast::ClassMethod>* methods = methodsByIndex_[i];
        if (methods == nullptr) continue;
        classes_[i].fieldTypes.clear();
        const std::map<std::string, Type>* ctorParams = nullptr;
        if (byClass != nullptr) {
            const auto it = byClass->find(classes_[i].name);
            if (it != byClass->end()) ctorParams = &it->second;
        }
        for (const auto& m : *methods) {
            if (m.isStatic) continue;
            if (m.fn) {
                FieldTypeWalker walker(classes_[i].fieldTypes, byName_, classes_,
                                       m.isConstructor ? ctorParams : nullptr);
                walker.walkList(m.fn->body);
            } else if (m.init && !m.name.empty()) {
                classes_[i].fieldTypes[m.name] =
                    harvestFieldType(*m.init, byName_, classes_, nullptr);
            }
        }
    }
    // Inheritance runs only for the re-harvest, which is to say only when the
    // constructor-parameter mechanism is on (`BRONZE_NO_CTOR_PARAM_TYPES`). It
    // is the other half of what a `super(...)` buys: a parameter carried into a
    // base's field is read back through a SUBCLASS instance, and without this
    // the read finds no entry and answers `dynamic`. With the seam set, the
    // syntactic harvest `build` runs is left exactly as it was.
    if (byClass == nullptr) return;
    std::vector<bool> done(classes_.size(), false);
    for (size_t i = 0; i < classes_.size(); ++i) inheritFieldTypes(i, done);
}

// A subclass's instance carries its base's fields — `ClassLayout::fields`
// records exactly that, base prefix first — so the base's harvest describes
// slots a subclass receiver really has. Without this the table said two
// different things about one object: `Mesh` has a slot called `position`, and
// nothing is known about what a `Mesh`'s `position` holds.
//
// Joined rather than overwritten in either direction. A derived class that
// re-assigns an inherited name writes into the SAME slot the base wrote, so the
// slot can hold either, and only the join describes both.
void ClassLayoutTable::inheritFieldTypes(size_t index, std::vector<bool>& done) {
    if (done[index]) return;
    // Set before recursing: a cyclic `extends` is a runtime TypeError and must
    // not be an infinite loop here.
    done[index] = true;
    const std::string super = classes_[index].superName;
    if (super.empty()) return;
    const auto it = byName_.find(super);
    if (it == byName_.end() || it->second == index) return;
    inheritFieldTypes(it->second, done);
    for (const auto& entry : classes_[it->second].fieldTypes) {
        auto& own = classes_[index].fieldTypes;
        const auto found = own.find(entry.first);
        own[entry.first] = found == own.end() ? entry.second
                                              : join(found->second, entry.second);
    }
}

bool ClassLayoutTable::reharvestFieldTypes(
    const std::map<std::string, std::map<std::string, Type>>& byClass) {
    std::vector<std::map<std::string, Type>> before;
    before.reserve(classes_.size());
    for (const auto& cl : classes_) before.push_back(cl.fieldTypes);
    harvestFieldTypes(&byClass);
    for (size_t i = 0; i < classes_.size(); ++i) {
        if (classes_[i].fieldTypes != before[i]) return true;
    }
    return false;
}

}  // namespace bronze::types
