// The field-type half of `ClassLayoutTable`: not which slots an instance has,
// which is `class_layout.cpp`'s question, but what those slots HOLD.

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "types/class_layout.h"
#include "types/walk.h"

namespace bronze::types {
namespace {

Type harvestFieldType(const ast::Expr& rhs, const std::map<std::string, size_t>& classByName,
                      const std::vector<ClassLayout>& classes,
                      const std::map<std::string, Type>* paramTypes) {
    if (paramTypes != nullptr) {
        if (const auto* id = dynamic_cast<const ast::Ident*>(&rhs)) {
            const auto it = paramTypes->find(id->name);
            if (it != paramTypes->end()) return it->second;
        }
    }
    if (dynamic_cast<const ast::NumberLit*>(&rhs)) return Type::number();
    if (dynamic_cast<const ast::StringLit*>(&rhs)) return Type::string();
    if (dynamic_cast<const ast::BoolLit*>(&rhs)) return Type::boolean();
    if (dynamic_cast<const ast::NullLit*>(&rhs)) return Type::null();
    if (dynamic_cast<const ast::UndefinedLit*>(&rhs)) return Type::undefined();
    if (const auto* u = dynamic_cast<const ast::Unary*>(&rhs)) {
        if (u->op == ast::UnaryOp::Negate || u->op == ast::UnaryOp::Posate ||
            u->op == ast::UnaryOp::BitNot) {
            const Type opT = harvestFieldType(*u->operand, classByName, classes, paramTypes);
            if (opT.is(TypeKind::Number)) return Type::number();
        }
        return Type::dynamic();
    }
    if (const auto* b = dynamic_cast<const ast::Binary*>(&rhs)) {
        if (b->op == ast::BinaryOp::Add || b->op == ast::BinaryOp::Sub ||
            b->op == ast::BinaryOp::Mul || b->op == ast::BinaryOp::Div ||
            b->op == ast::BinaryOp::Mod || b->op == ast::BinaryOp::BitAnd ||
            b->op == ast::BinaryOp::BitOr || b->op == ast::BinaryOp::BitXor ||
            b->op == ast::BinaryOp::Shl || b->op == ast::BinaryOp::Shr ||
            b->op == ast::BinaryOp::UShr || b->op == ast::BinaryOp::Exp) {
            const Type lhsT = harvestFieldType(*b->lhs, classByName, classes, paramTypes);
            const Type rhsT = harvestFieldType(*b->rhs, classByName, classes, paramTypes);
            if (lhsT.is(TypeKind::Number) && rhsT.is(TypeKind::Number)) return Type::number();
            if (b->op != ast::BinaryOp::Add &&
                (lhsT.is(TypeKind::Number) || rhsT.is(TypeKind::Number))) {
                return Type::number();
            }
        }
    }
    if (const auto* m = dynamic_cast<const ast::MemberAccess*>(&rhs)) {
        if (paramTypes != nullptr) {
            if (const auto* id = dynamic_cast<const ast::Ident*>(m->object.get())) {
                const auto it = paramTypes->find(id->name);
                if (it != paramTypes->end() && it->second.is(TypeKind::Object)) {
                    const ShapeClassId cls = it->second.shapeClass();
                    for (const auto& cl : classes) {
                        if (cl.shapeClass == cls) {
                            const auto fit = cl.fieldTypes.find(m->property);
                            if (fit != cl.fieldTypes.end()) return fit->second;
                        }
                    }
                }
            }
        }
    }
    if (const auto* c = dynamic_cast<const ast::Call*>(&rhs)) {
        if (const auto* m = dynamic_cast<const ast::MemberAccess*>(c->callee.get())) {
            if (const auto* id = dynamic_cast<const ast::Ident*>(m->object.get())) {
                if (id->name == "Math") return Type::number();
            }
        }
    }
    if (const auto* t = dynamic_cast<const ast::Ternary*>(&rhs)) {
        const Type thenT = harvestFieldType(*t->thenExpr, classByName, classes, paramTypes);
        const Type elseT = harvestFieldType(*t->elseExpr, classByName, classes, paramTypes);
        return join(thenT, elseT);
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

class FieldTypeWalker final : public Walker {
public:
    FieldTypeWalker(std::map<std::string, Type>& out,
                    const std::map<std::string, size_t>& classByName,
                    const std::vector<ClassLayout>& classes,
                    const std::map<std::string, Type>* paramTypes = nullptr)
        : out_(out), classByName_(classByName), classes_(classes), paramTypes_(paramTypes) {}

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
                           harvestFieldType(*n.rhs, classByName_, classes_, paramTypes_));
                }
            }
        } else if (ast::isAssignOp(n.op)) {
            if (const auto* m = dynamic_cast<const ast::MemberAccess*>(n.lhs.get())) {
                if (dynamic_cast<const ast::ThisExpr*>(m->object.get()) && !m->isPrivate) {
                    const Type rhsT = harvestFieldType(*n.rhs, classByName_, classes_, paramTypes_);
                    const auto it = out_.find(m->property);
                    if (it != out_.end() && it->second.is(TypeKind::Number) && rhsT.is(TypeKind::Number)) {
                        record(m->property, Type::number());
                    } else {
                        record(m->property, Type::dynamic());
                    }
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
    const std::map<std::string, Type>* paramTypes_ = nullptr;
};

}  // namespace

void ClassLayoutTable::harvestFieldTypes(
    const std::map<std::string, std::map<std::string, Type>>* byClass,
    const std::map<std::string, std::map<std::string, std::map<std::string, Type>>>* methodByClass) {
    for (size_t i = 0; i < classes_.size(); ++i) {
        const std::vector<ast::ClassMethod>* methods = methodsByIndex_[i];
        if (methods == nullptr) continue;
        classes_[i].fieldTypes.clear();
        const std::map<std::string, Type>* ctorParams = nullptr;
        if (byClass != nullptr) {
            const auto it = byClass->find(classes_[i].name);
            if (it != byClass->end()) ctorParams = &it->second;
        }
        const std::map<std::string, std::map<std::string, Type>>* classMethods = nullptr;
        if (methodByClass != nullptr) {
            const auto it = methodByClass->find(classes_[i].name);
            if (it != methodByClass->end()) classMethods = &it->second;
        }
        for (const auto& m : *methods) {
            if (m.isStatic) continue;
            if (m.fn) {
                const std::map<std::string, Type>* fnParams = nullptr;
                if (m.isConstructor) {
                    fnParams = ctorParams;
                } else if (classMethods != nullptr) {
                    const auto it = classMethods->find(m.name);
                    if (it != classMethods->end()) fnParams = &it->second;
                }
                FieldTypeWalker walker(classes_[i].fieldTypes, byName_, classes_, fnParams);
                walker.walkList(m.fn->body);
            } else if (m.init && !m.name.empty()) {
                classes_[i].fieldTypes[m.name] =
                    harvestFieldType(*m.init, byName_, classes_, nullptr);
            }
        }
    }
    if (byClass == nullptr && methodByClass == nullptr) return;
    std::vector<bool> done(classes_.size(), false);
    for (size_t i = 0; i < classes_.size(); ++i) inheritFieldTypes(i, done);
}

void ClassLayoutTable::inheritFieldTypes(size_t index, std::vector<bool>& done) {
    if (done[index]) return;
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
    const std::map<std::string, std::map<std::string, Type>>& byClass,
    const std::map<std::string, std::map<std::string, std::map<std::string, Type>>>* methodByClass) {
    std::vector<std::map<std::string, Type>> before;
    before.reserve(classes_.size());
    for (const auto& cl : classes_) before.push_back(cl.fieldTypes);
    harvestFieldTypes(&byClass, methodByClass);
    for (size_t i = 0; i < classes_.size(); ++i) {
        if (classes_[i].fieldTypes != before[i]) return true;
    }
    return false;
}

}  // namespace bronze::types
