// The field-type half of `ClassLayoutTable`: not which slots an instance has,
// which is `class_layout.cpp`'s question, but what those slots HOLD.

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "types/class_layout.h"
#include "types/math_builtins.h"
#include "types/operator_types.h"
#include "types/walk.h"

namespace bronze::types {
namespace {

Type harvestFieldType(const ast::Expr& rhs, const std::map<std::string, size_t>& classByName,
                      const std::vector<ClassLayout>& classes,
                      const std::map<std::string, Type>* paramTypes,
                      bool mathPristine = false,
                      ShapeClassTable* shapes = nullptr,
                      const std::map<std::string, Type>* localConsts = nullptr) {
    if (const auto* id = dynamic_cast<const ast::Ident*>(&rhs)) {
        if (localConsts != nullptr) {
            const auto it = localConsts->find(id->name);
            if (it != localConsts->end()) return it->second;
        }
        if (paramTypes != nullptr) {
            const auto it = paramTypes->find(id->name);
            if (it != paramTypes->end()) return it->second;
        }
    }
    if (dynamic_cast<const ast::NumberLit*>(&rhs)) return Type::number();
    if (dynamic_cast<const ast::StringLit*>(&rhs)) return Type::string();
    if (dynamic_cast<const ast::BoolLit*>(&rhs)) return Type::boolean();
    if (dynamic_cast<const ast::NullLit*>(&rhs)) return Type::null();
    if (dynamic_cast<const ast::UndefinedLit*>(&rhs)) return Type::undefined();
    if (const auto* o = dynamic_cast<const ast::ObjectLit*>(&rhs)) {
        if (shapes == nullptr) return Type::dynamic();
        std::vector<std::string> props;
        for (const auto& p : o->props) {
            if (p.keyExpr || dynamic_cast<const ast::SpreadElement*>(p.value.get()) != nullptr) {
                return Type::dynamic();
            }
            if (std::find(props.begin(), props.end(), p.key) == props.end()) {
                props.push_back(p.key);
            }
        }
        const ShapeClassId cls = shapes->intern(std::string(), std::move(props));
        return cls == kNoShapeClass ? Type::dynamic() : Type::object(cls);
    }
    if (const auto* a = dynamic_cast<const ast::ArrayLit*>(&rhs)) {
        if (!a->elements.empty()) {
            bool canTrack = true;
            bool allNever = true;
            ShapeClassId commonCls = kNoShapeClass;
            for (const auto& el : a->elements) {
                if (!el || dynamic_cast<const ast::SpreadElement*>(el.get()) != nullptr) {
                    canTrack = false;
                    break;
                }
                const Type elT = harvestFieldType(*el, classByName, classes, paramTypes, mathPristine, shapes);
                if (elT.is(TypeKind::Never)) continue;
                allNever = false;
                if (elT.is(TypeKind::Object) && elT.shapeClass() != kNoShapeClass) {
                    if (commonCls == kNoShapeClass) {
                        commonCls = elT.shapeClass();
                    } else if (commonCls != elT.shapeClass()) {
                        canTrack = false;
                        break;
                    }
                } else {
                    canTrack = false;
                    break;
                }
            }
            if (canTrack) {
                if (allNever) return Type::arrayOfNever();
                if (commonCls != kNoShapeClass) return Type::arrayOfObject(commonCls);
            }
        }
        return Type::array();
    }
    if (const auto* u = dynamic_cast<const ast::Unary*>(&rhs)) {
        if (u->op == ast::UnaryOp::Negate || u->op == ast::UnaryOp::Posate ||
            u->op == ast::UnaryOp::BitNot) {
            const Type opT = harvestFieldType(*u->operand, classByName, classes, paramTypes, mathPristine, shapes);
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
            const Type lhsT = harvestFieldType(*b->lhs, classByName, classes, paramTypes, mathPristine, shapes);
            const Type rhsT = harvestFieldType(*b->rhs, classByName, classes, paramTypes, mathPristine, shapes);
            if (lhsT.is(TypeKind::Number) && rhsT.is(TypeKind::Number)) return Type::number();
            if (b->op != ast::BinaryOp::Add &&
                isNeverBigInt(lhsT) && isNeverBigInt(rhsT) &&
                (lhsT.is(TypeKind::Number) || rhsT.is(TypeKind::Number))) {
                return Type::number();
            }
        }
    }
    if (const auto* m = dynamic_cast<const ast::MemberAccess*>(&rhs)) {
        if (mathPristine && !m->optional) {
            if (const auto* id = dynamic_cast<const ast::Ident*>(m->object.get())) {
                if (id->name == "Math" && isMathValueProperty(m->property)) {
                    return Type::number();
                }
            }
        }
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
        if (mathPristine && !c->optional) {
            if (const auto* m = dynamic_cast<const ast::MemberAccess*>(c->callee.get())) {
                if (!m->optional) {
                    if (const auto* id = dynamic_cast<const ast::Ident*>(m->object.get())) {
                        if (id->name == "Math" && isMathMethodReturningNumber(m->property)) {
                            return Type::number();
                        }
                    }
                }
            }
        }
    }
    if (const auto* t = dynamic_cast<const ast::Ternary*>(&rhs)) {
        const Type thenT = harvestFieldType(*t->thenExpr, classByName, classes, paramTypes, mathPristine, shapes);
        const Type elseT = harvestFieldType(*t->elseExpr, classByName, classes, paramTypes, mathPristine, shapes);
        return join(thenT, elseT);
    }
    if (const auto* n = dynamic_cast<const ast::NewExpr*>(&rhs)) {
        const auto* id = dynamic_cast<const ast::Ident*>(n->callee.get());
        if (id == nullptr) return Type::dynamic();
        if (id->name == "Float64Array") return Type::typedArray(TypedArrayElem::Float64);
        if (id->name == "Float32Array") return Type::typedArray(TypedArrayElem::Float32);
        if (id->name == "Int32Array") return Type::typedArray(TypedArrayElem::Int32);
        if (id->name == "Uint32Array") return Type::typedArray(TypedArrayElem::Uint32);
        if (id->name == "Int16Array") return Type::typedArray(TypedArrayElem::Int16);
        if (id->name == "Uint16Array") return Type::typedArray(TypedArrayElem::Uint16);
        if (id->name == "Int8Array") return Type::typedArray(TypedArrayElem::Int8);
        if (id->name == "Uint8Array") return Type::typedArray(TypedArrayElem::Uint8);
        if (id->name == "Uint8ClampedArray") return Type::typedArray(TypedArrayElem::Uint8Clamped);
        if (id->name == "Array") return Type::array();
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
                    const std::map<std::string, Type>* paramTypes = nullptr,
                    bool mathPristine = false,
                    bool isCtor = true,
                    std::set<std::string>* emptyArrayInit = nullptr,
                    std::map<std::string, Type>* pushedElemTypes = nullptr,
                    std::set<std::string>* sawPush = nullptr,
                    ShapeClassTable* shapes = nullptr)
        : out_(out), classByName_(classByName), classes_(classes), paramTypes_(paramTypes),
          mathPristine_(mathPristine), isCtor_(isCtor), emptyArrayInit_(emptyArrayInit),
          pushedElemTypes_(pushedElemTypes), sawPush_(sawPush), shapes_(shapes) {}

    void visit(const ast::FunctionExpr& n) override {
        if (n.isArrow) Walker::visit(n);
    }
    void visit(const ast::FunctionDecl&) override {}
    void visit(const ast::ClassDecl&) override {}
    void visit(const ast::ClassExpr&) override {}

    void visit(const ast::VarDecl& n) override {
        if (n.isConst && !n.pattern && !n.name.empty() && n.init != nullptr) {
            localConsts_[n.name] = harvestFieldType(*n.init, classByName_, classes_, paramTypes_,
                                                    mathPristine_, shapes_, &localConsts_);
        }
        Walker::visit(n);
    }

    void visit(const ast::Call& n) override {
        if (!n.optional) {
            if (const auto* calleeMem = dynamic_cast<const ast::MemberAccess*>(n.callee.get())) {
                if (!calleeMem->optional) {
                    if (const auto* baseId = dynamic_cast<const ast::Ident*>(calleeMem->object.get())) {
                        if ((baseId->name == "Object" || baseId->name == "Reflect") &&
                            n.args.size() >= 2 &&
                            dynamic_cast<const ast::ThisExpr*>(n.args[0].get()) != nullptr) {
                            if (calleeMem->property == "defineProperty" && n.args.size() >= 3) {
                                if (const auto* key = dynamic_cast<const ast::StringLit*>(n.args[1].get())) {
                                    harvestDescriptor(key->value, n.args[2].get());
                                }
                            } else if (calleeMem->property == "defineProperties") {
                                if (const auto* map = dynamic_cast<const ast::ObjectLit*>(n.args[1].get())) {
                                    for (const auto& p : map->props) {
                                        if (!p.computed() && !p.key.empty() &&
                                            p.accessor == ast::AccessorKind::None && p.value) {
                                            harvestDescriptor(p.key, p.value.get());
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if (pushedElemTypes_ != nullptr && sawPush_ != nullptr &&
                        (calleeMem->property == "push" || calleeMem->property == "unshift")) {
                        if (const auto* fieldMem =
                                dynamic_cast<const ast::MemberAccess*>(calleeMem->object.get())) {
                            if (!fieldMem->isPrivate && !fieldMem->optional &&
                                dynamic_cast<const ast::ThisExpr*>(fieldMem->object.get()) != nullptr) {
                                sawPush_->insert(fieldMem->property);
                                Type& acc = (*pushedElemTypes_)[fieldMem->property];
                                for (const auto& arg : n.args) {
                                    if (!arg || dynamic_cast<const ast::SpreadElement*>(arg.get()) != nullptr) {
                                        acc = Type::dynamic();
                                    } else {
                                        acc = join(acc, harvestFieldType(*arg, classByName_, classes_,
                                                                         paramTypes_, mathPristine_,
                                                                         shapes_, &localConsts_));
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        Walker::visit(n);
    }

    void visit(const ast::Binary& n) override {
        if (n.op == ast::BinaryOp::Assign) {
            if (const auto* m = dynamic_cast<const ast::MemberAccess*>(n.lhs.get())) {
                if (dynamic_cast<const ast::ThisExpr*>(m->object.get()) && !m->isPrivate) {
                    if (emptyArrayInit_ != nullptr) {
                        const auto* arr = dynamic_cast<const ast::ArrayLit*>(n.rhs.get());
                        if (isCtor_ && arr != nullptr && arr->elements.empty() &&
                            out_.count(m->property) == 0) {
                            emptyArrayInit_->insert(m->property);
                        } else {
                            emptyArrayInit_->erase(m->property);
                        }
                    }
                    record(m->property,
                           harvestFieldType(*n.rhs, classByName_, classes_, paramTypes_,
                                            mathPristine_, shapes_, &localConsts_));
                }
            }
        } else if (ast::isAssignOp(n.op)) {
            if (const auto* m = dynamic_cast<const ast::MemberAccess*>(n.lhs.get())) {
                if (dynamic_cast<const ast::ThisExpr*>(m->object.get()) && !m->isPrivate) {
                    if (emptyArrayInit_ != nullptr) emptyArrayInit_->erase(m->property);
                    const Type rhsT = harvestFieldType(*n.rhs, classByName_, classes_, paramTypes_,
                                                       mathPristine_, shapes_, &localConsts_);
                    const auto it = out_.find(m->property);
                    if (it != out_.end() && it->second.is(TypeKind::Number) && rhsT.is(TypeKind::Number)) {
                        record(m->property, Type::number());
                    } else if (it != out_.end() && (rhsT.is(TypeKind::Never) || (!isCtor_ && rhsT.is(TypeKind::Dynamic)))) {
                        // Do not clobber a constructor-installed field type from a non-constructor
                        // compound assignment whose local RHS cannot be resolved syntactically.
                    } else {
                        record(m->property, Type::dynamic());
                    }
                }
            }
        }
        Walker::visit(n);
    }

private:
    void harvestDescriptor(const std::string& propName, const ast::Expr* desc) {
        const auto* lit = dynamic_cast<const ast::ObjectLit*>(desc);
        if (lit == nullptr) return;
        const ast::Expr* valExpr = nullptr;
        for (const auto& p : lit->props) {
            if (p.computed() || p.accessor != ast::AccessorKind::None || p.coverInitialized) return;
            if (p.key == "get" || p.key == "set" || p.key.empty()) return;
            if (p.key == "value") valExpr = p.value.get();
        }
        if (valExpr == nullptr) return;
        if (emptyArrayInit_ != nullptr) emptyArrayInit_->erase(propName);
        record(propName, harvestFieldType(*valExpr, classByName_, classes_, paramTypes_,
                                          mathPristine_, shapes_, &localConsts_));
    }

    void record(const std::string& name, Type t) {
        const auto it = out_.find(name);
        if (it == out_.end()) {
            out_[name] = t;
            return;
        }
        if (!isCtor_ && (t.is(TypeKind::Dynamic) || t.is(TypeKind::Never))) {
            return;
        }
        out_[name] = join(it->second, t);
    }
    std::map<std::string, Type>& out_;
    const std::map<std::string, size_t>& classByName_;
    const std::vector<ClassLayout>& classes_;
    const std::map<std::string, Type>* paramTypes_ = nullptr;
    bool mathPristine_ = false;
    bool isCtor_ = true;
    std::set<std::string>* emptyArrayInit_ = nullptr;
    std::map<std::string, Type>* pushedElemTypes_ = nullptr;
    std::set<std::string>* sawPush_ = nullptr;
    ShapeClassTable* shapes_ = nullptr;
    std::map<std::string, Type> localConsts_;
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
        std::set<std::string> emptyArrayInit;
        std::map<std::string, Type> pushedElemTypes;
        std::set<std::string> sawPush;
        for (const auto& m : *methods) {
            if (m.isStatic) continue;
            if (m.fn && m.isConstructor) {
                FieldTypeWalker walker(classes_[i].fieldTypes, byName_, classes_, ctorParams,
                                       mathPristine_, true, &emptyArrayInit, &pushedElemTypes, &sawPush, shapes_);
                walker.walkList(m.fn->body);
            } else if (!m.fn && m.init && !m.name.empty()) {
                const auto* arr = dynamic_cast<const ast::ArrayLit*>(m.init.get());
                if (arr != nullptr && arr->elements.empty() &&
                    classes_[i].fieldTypes.count(m.name) == 0) {
                    emptyArrayInit.insert(m.name);
                } else {
                    emptyArrayInit.erase(m.name);
                }
                classes_[i].fieldTypes[m.name] =
                    harvestFieldType(*m.init, byName_, classes_, nullptr, mathPristine_, shapes_);
            }
        }
        for (const auto& m : *methods) {
            if (m.isStatic || m.isConstructor || !m.fn || m.accessor != ast::AccessorKind::None) continue;
            const std::map<std::string, Type>* fnParams = nullptr;
            if (classMethods != nullptr) {
                const auto it = classMethods->find(m.name);
                if (it != classMethods->end()) fnParams = &it->second;
            }
            FieldTypeWalker walker(classes_[i].fieldTypes, byName_, classes_, fnParams,
                                   mathPristine_, false, &emptyArrayInit, &pushedElemTypes, &sawPush, shapes_);
            walker.walkList(m.fn->body);
        }
        for (const auto& prop : sawPush) {
            if (emptyArrayInit.count(prop) == 0) continue;
            const Type elemT = pushedElemTypes[prop];
            if (elemT.is(TypeKind::Never) && methodByClass != nullptr) {
                classes_[i].fieldTypes[prop] = Type::arrayOfNever();
            } else if (elemT.is(TypeKind::Object) && elemT.shapeClass() != kNoShapeClass) {
                classes_[i].fieldTypes[prop] = Type::arrayOfObject(elemT.shapeClass());
            } else {
                classes_[i].fieldTypes[prop] = Type::array();
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
