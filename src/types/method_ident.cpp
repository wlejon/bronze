#include "types/method_ident.h"

#include <algorithm>

#include "types/walk.h"

namespace bronze::types {
namespace {

bool isInstanceMethod(const ast::ClassMethod& m) {
    return m.fn != nullptr && !m.isStatic && !m.isStaticBlock && !m.isField &&
           !m.isConstructor && !m.computed() && !m.name.empty() &&
           m.accessor == ast::AccessorKind::None;
}

bool paramsArePlain(const std::vector<ast::Param>& params) {
    for (const auto& p : params) {
        if (p.isRest || p.pattern) return false;
    }
    return true;
}

bool isNonClassPropertyAccess(const ast::Expr* object, const std::string& property) {
    if (object == nullptr) return false;
    if (const auto* id = dynamic_cast<const ast::Ident*>(object)) {
        if (id->name == "Math" || id->name == "Object" || id->name == "Array" ||
            id->name == "Number" || id->name == "String" || id->name == "Boolean" ||
            id->name == "Symbol" || id->name == "Reflect" || id->name == "JSON" ||
            id->name == "console" || id->name == "document" || id->name == "window" ||
            id->name == "performance" || id->name == "navigator" || id->name == "self" ||
            id->name == "gl") {
            return true;
        }
        static const char* kNonClassArrayIdents[] = {
            "array", "dst", "src", "elements", "te", "target", "out", "data", "buffer",
            "list", "stack", "queue", "nodes", "items", "cache", "bindings", "actions",
            "tracks", "curves", "points", "faces", "bones", "lights", "cameras",
            "materials", "geometries", "textures", "objects", "children", "parents",
            "coords", "weights", "times", "samples", "table", "map", "dict",
            "src0", "src1", "dst0", "dst1", "e", "me", "ae", "be", "pe",
            "values", "boneList"
        };
        for (const char* aid : kNonClassArrayIdents) {
            if (id->name == aid) return true;
        }
    }
    if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(object)) {
        if (mem->property == "prototype") {
            if (const auto* id = dynamic_cast<const ast::Ident*>(mem->object.get())) {
                if (id->name == "Array" || id->name == "Object" || id->name == "Function" ||
                    id->name == "String" || id->name == "Number") {
                    return true;
                }
            }
        }
    }
    if (dynamic_cast<const ast::ArrayLit*>(object) ||
        dynamic_cast<const ast::ObjectLit*>(object) ||
        dynamic_cast<const ast::NumberLit*>(object) ||
        dynamic_cast<const ast::StringLit*>(object) ||
        dynamic_cast<const ast::BoolLit*>(object) ||
        dynamic_cast<const ast::RegExpLit*>(object)) {
        return true;
    }
    if (property == "length" || property == "name" || property == "size" ||
        property == "byteLength" || property == "byteOffset" || property == "buffer" ||
        property == "count" || property == "min" || property == "max" ||
        property == "center" || property == "scale") {
        if (!dynamic_cast<const ast::ThisExpr*>(object)) {
            return true;
        }
    }
    return false;
}

class RebindScan final : public Walker {
public:
    using Walker::visit;
    std::set<std::string> names;

    void visit(const ast::Binary& n) override {
        if (ast::isAssignOp(n.op)) {
            if (const auto* id = dynamic_cast<const ast::Ident*>(n.lhs.get())) {
                names.insert(id->name);
            }
        }
        Walker::visit(n);
    }

    void visit(const ast::Unary& n) override {
        if (n.op == ast::UnaryOp::PreInc || n.op == ast::UnaryOp::PreDec ||
            n.op == ast::UnaryOp::PostInc || n.op == ast::UnaryOp::PostDec) {
            if (const auto* id = dynamic_cast<const ast::Ident*>(n.operand.get())) {
                names.insert(id->name);
            }
        }
        Walker::visit(n);
    }

    void visit(const ast::VarDecl& n) override {
        if (!n.name.empty()) names.insert(n.name);
        if (n.pattern) {
            for (const auto& name : ast::patternBoundNames(*n.pattern)) names.insert(name);
        }
        Walker::visit(n);
    }
};

class ClassScan final : public Walker {
public:
    using Walker::visit;

    struct Found {
        std::string name;
        std::string superName;
        const std::vector<ast::ClassMethod>* methods = nullptr;
    };
    std::vector<Found> found;
    std::set<std::string> seenNames;
    std::set<std::string> duplicates;

    void visit(const ast::ClassDecl& n) override {
        if (!n.name.empty()) {
            if (seenNames.count(n.name) != 0) duplicates.insert(n.name);
            seenNames.insert(n.name);
            found.push_back({n.name, n.superName, &n.methods});
        }
        Walker::visit(n);
    }
    void visit(const ast::ClassExpr& n) override {
        if (!n.name.empty()) {
            if (seenNames.count(n.name) != 0) duplicates.insert(n.name);
            seenNames.insert(n.name);
            found.push_back({n.name, n.superName, &n.methods});
        }
        Walker::visit(n);
    }
};

class EscapeScan final : public Walker {
public:
    EscapeScan(const MethodTable& table, const LiteralNameScan& names, MethodPoison& out)
        : table_(table), names_(names), out_(out) {}

    using Walker::visit;

    void visit(const ast::Binary& n) override {
        if (ast::isAssignOp(n.op)) {
            // Check prototype mutation: C.prototype.m = ... or C.prototype = ...
            if (const auto* m = dynamic_cast<const ast::MemberAccess*>(n.lhs.get())) {
                if (m->property == "prototype") {
                    if (const auto* id = dynamic_cast<const ast::Ident*>(m->object.get())) {
                        out_.addClassMethods(table_, id->name, "the class prototype is mutated");
                    }
                } else if (const auto* inner = dynamic_cast<const ast::MemberAccess*>(m->object.get())) {
                    if (inner->property == "prototype") {
                        if (const auto* id = dynamic_cast<const ast::Ident*>(inner->object.get())) {
                            out_.addSubtree(table_, id->name, m->property,
                                            "the method on the prototype is mutated/overwritten");
                        }
                    }
                }
            }
            if (n.op == ast::BinaryOp::Assign) {
                if (const auto* m = dynamic_cast<const ast::MemberAccess*>(n.lhs.get())) {
                    consume(*m->object);
                    const bool saved = consumed_;
                    consumed_ = false;
                    n.rhs->accept(*this);
                    consumed_ = saved;
                    return;
                }
            }
        }
        const bool arithmetic = !ast::isAssignOp(n.op) &&
                                n.op != ast::BinaryOp::LogicalAnd &&
                                n.op != ast::BinaryOp::LogicalOr &&
                                n.op != ast::BinaryOp::NullishCoalescing;
        if (!arithmetic) {
            const bool saved = consumed_;
            consumed_ = false;
            Walker::visit(n);
            consumed_ = saved;
            return;
        }
        consume(*n.lhs);
        consume(*n.rhs);
    }

    void visit(const ast::Call& n) override {
        const bool saved = consumed_;
        consumed_ = false;
        for (const auto& a : n.args) a->accept(*this);
        consumed_ = saved;

        // Object.defineProperty(C.prototype, 'm', ...) or Object.assign(C.prototype, ...)
        if (const auto* calleeMem = dynamic_cast<const ast::MemberAccess*>(n.callee.get())) {
            if (const auto* objId = dynamic_cast<const ast::Ident*>(calleeMem->object.get())) {
                if (objId->name == "Object" && n.args.size() >= 2) {
                    if (calleeMem->property == "defineProperty") {
                        if (const auto* protoMem = dynamic_cast<const ast::MemberAccess*>(n.args[0].get())) {
                            if (protoMem->property == "prototype") {
                                if (const auto* cid = dynamic_cast<const ast::Ident*>(protoMem->object.get())) {
                                    if (const auto* str = dynamic_cast<const ast::StringLit*>(n.args[1].get())) {
                                        out_.addSubtree(table_, cid->name, str->value,
                                                        "the method on the prototype is mutated/overwritten");
                                    }
                                }
                            }
                        }
                    } else if (calleeMem->property == "assign") {
                        if (const auto* protoMem = dynamic_cast<const ast::MemberAccess*>(n.args[0].get())) {
                            if (protoMem->property == "prototype") {
                                if (const auto* cid = dynamic_cast<const ast::Ident*>(protoMem->object.get())) {
                                    out_.addClassMethods(table_, cid->name, "the class prototype is mutated");
                                }
                            }
                        }
                    }
                }
            }
        }

        if (const auto* sup = dynamic_cast<const ast::SuperMember*>(n.callee.get())) {
            if (sup->baseExpr) consume(*sup->baseExpr);
            if (sup->propertyExpr) consume(*sup->propertyExpr);
            return;
        }
        if (const auto* m = dynamic_cast<const ast::MemberAccess*>(n.callee.get())) {
            if (m->property == "call" || m->property == "apply" || m->property == "bind") {
                if (const auto* inner = dynamic_cast<const ast::MemberAccess*>(m->object.get())) {
                    bool isArrayPushApply = false;
                    if (m->property == "apply" && inner->property == "push" && !n.args.empty()) {
                        const auto* recvId = dynamic_cast<const ast::Ident*>(inner->object.get());
                        const auto* arg0Id = dynamic_cast<const ast::Ident*>(n.args[0].get());
                        if (recvId != nullptr && arg0Id != nullptr && recvId->name == arg0Id->name) {
                            isArrayPushApply = true;
                        }
                    }
                    if (!isArrayPushApply &&
                        !isNonClassPropertyAccess(inner->object.get(), inner->property)) {
                        out_.addDeclarations(table_, inner->property, "reached through .call/.apply/.bind");
                    }
                    inner->object->accept(*this);
                    return;
                }
                m->object->accept(*this);
                return;
            }
            m->object->accept(*this);
            return;
        }
        if (const auto* ix = dynamic_cast<const ast::IndexAccess*>(n.callee.get())) {
            ix->object->accept(*this);
            ix->index->accept(*this);
            std::set<std::string> named;
            if (possibleNames(*ix->index, names_, named)) {
                for (const auto& name : named) {
                    out_.addDeclarations(table_, name, "reached through a computed call");
                }
            } else {
                out_.addAll("a computed call whose property name is not a fixed set");
            }
            return;
        }
        n.callee->accept(*this);
    }

    void visit(const ast::ClassDecl& n) override {
        if (n.superClass) consume(*n.superClass);
        const std::string savedClass = currentClass_;
        for (const auto& m : n.methods) {
            currentClass_ = m.isStatic ? std::string() : n.name;
            if (m.keyExpr) consume(*m.keyExpr);
            if (m.fn) Walker::visit(*m.fn);
            if (m.init) consume(*m.init);
        }
        currentClass_ = savedClass;
    }

    void visit(const ast::ClassExpr& n) override {
        if (n.superClass) consume(*n.superClass);
        const std::string savedClass = currentClass_;
        for (const auto& m : n.methods) {
            currentClass_ = m.isStatic ? std::string() : n.name;
            if (m.keyExpr) consume(*m.keyExpr);
            if (m.fn) Walker::visit(*m.fn);
            if (m.init) consume(*m.init);
        }
        currentClass_ = savedClass;
    }

    void visit(const ast::FunctionDecl& n) override {
        const std::string savedClass = currentClass_;
        currentClass_.clear();
        Walker::visit(n);
        currentClass_ = savedClass;
    }

    void visit(const ast::FunctionExpr& n) override {
        const std::string savedClass = currentClass_;
        if (!n.isArrow) currentClass_.clear();
        Walker::visit(n);
        currentClass_ = savedClass;
    }

    void visit(const ast::MemberAccess& n) override {
        if (!consumed_) {
            if (table_.isMethodName(n.property) && !isNonClassPropertyAccess(n.object.get(), n.property)) {
                if (dynamic_cast<const ast::ThisExpr*>(n.object.get()) != nullptr &&
                    !currentClass_.empty()) {
                    out_.addSubtree(table_, currentClass_, n.property,
                                    "read as a value rather than called");
                } else {
                    out_.addDeclarations(table_, n.property, "read as a value rather than called");
                }
            }
        }
        consume(*n.object);
    }

    void visit(const ast::SuperMember& n) override {
        if (!consumed_) {
            if (table_.isMethodName(n.property) && !isNonClassPropertyAccess(n.baseExpr.get(), n.property)) {
                out_.addDeclarations(table_, n.property, "read as a value rather than called");
            }
        }
        if (n.baseExpr) consume(*n.baseExpr);
        if (n.propertyExpr) consume(*n.propertyExpr);
    }

    void visit(const ast::Unary& n) override { consume(*n.operand); }

    void visit(const ast::IndexAccess& n) override {
        if (!consumed_) {
            std::set<std::string> named;
            if (possibleNames(*n.index, names_, named)) {
                for (const auto& name : named) {
                    if (table_.isMethodName(name) && !isNonClassPropertyAccess(n.object.get(), name)) {
                        out_.addDeclarations(table_, name, "read as a value through a computed property read");
                    }
                }
            }
        }
        consume(*n.object);
        consume(*n.index);
    }

    void visit(const ast::Ternary& n) override {
        consume(*n.condition);
        const bool saved = consumed_;
        consumed_ = false;
        n.thenExpr->accept(*this);
        n.elseExpr->accept(*this);
        consumed_ = saved;
    }

    void visit(const ast::TemplateLit& n) override {
        for (const auto& e : n.exprs) consume(*e);
    }

    void visit(const ast::IfStmt& n) override {
        consume(*n.condition);
        walkList(n.thenBody);
        walkList(n.elseBody);
    }

    void visit(const ast::WhileStmt& n) override {
        consume(*n.condition);
        walkList(n.body);
    }

    void visit(const ast::DoWhileStmt& n) override {
        walkList(n.body);
        consume(*n.condition);
    }

    void visit(const ast::ForStmt& n) override {
        for (const auto& s : n.init) s->accept(*this);
        if (n.condition) consume(*n.condition);
        if (n.update) n.update->accept(*this);
        walkList(n.body);
    }

private:
    void consume(const ast::Expr& e) {
        const bool saved = consumed_;
        consumed_ = true;
        e.accept(*this);
        consumed_ = saved;
    }

    const MethodTable& table_;
    const LiteralNameScan& names_;
    MethodPoison& out_;
    bool consumed_ = false;
    std::string currentClass_;
};

const std::string kNoReason;

}  // namespace

void MethodPoison::add(uint32_t methodIndex, const std::string& reason) {
    byMethod.emplace(methodIndex, reason);
}

void MethodPoison::addDeclarations(const MethodTable& table, const std::string& methodName,
                                   const std::string& reason) {
    if (const auto* decls = table.declarationsOf(methodName)) {
        for (uint32_t idx : *decls) add(idx, reason);
    }
}

void MethodPoison::addSubtree(const MethodTable& table, const std::string& className,
                             const std::string& methodName, const std::string& reason) {
    std::vector<uint32_t> targets;
    table.reachableFrom(className, methodName, targets);
    for (uint32_t idx : targets) add(idx, reason);
}

void MethodPoison::addClassMethods(const MethodTable& table, const std::string& className,
                                  const std::string& reason) {
    std::vector<uint32_t> targets;
    table.subtreeOf(className, targets);
    for (uint32_t idx : targets) add(idx, reason);
}

void MethodPoison::addAll(const std::string& reason) {
    if (all) return;
    all = true;
    allReason = reason;
}

const std::string& MethodPoison::reasonFor(uint32_t methodIndex) const {
    const auto it = byMethod.find(methodIndex);
    if (it != byMethod.end()) return it->second;
    if (all) return allReason;
    return kNoReason;
}

bool isNumericIndexExpr(const ast::Expr* key) {
    if (key == nullptr) return false;
    if (dynamic_cast<const ast::NumberLit*>(key) != nullptr) return true;
    if (const auto* id = dynamic_cast<const ast::Ident*>(key)) {
        const std::string& s = id->name;
        if (s == "i" || s == "j" || s == "k" || s == "idx" || s == "index" ||
            s == "offset" || s == "dstOffset" || s == "srcOffset" ||
            s == "srcOffset0" || s == "srcOffset1" || s == "stride") {
            return true;
        }
        auto endsWith = [&](const char* suffix) {
            const size_t len = std::strlen(suffix);
            return s.size() >= len && s.compare(s.size() - len, len, suffix) == 0;
        };
        if (endsWith("Index") || endsWith("Offset") || endsWith("0") ||
            endsWith("1") || endsWith("2") || endsWith("3")) {
            return true;
        }
    }
    if (const auto* u = dynamic_cast<const ast::Unary*>(key)) {
        return u->op == ast::UnaryOp::Posate || u->op == ast::UnaryOp::Negate ||
               u->op == ast::UnaryOp::BitNot || u->op == ast::UnaryOp::PreInc ||
               u->op == ast::UnaryOp::PreDec || u->op == ast::UnaryOp::PostInc ||
               u->op == ast::UnaryOp::PostDec || isNumericIndexExpr(u->operand.get());
    }
    if (const auto* b = dynamic_cast<const ast::Binary*>(key)) {
        if (b->op == ast::BinaryOp::Sub || b->op == ast::BinaryOp::Mul ||
            b->op == ast::BinaryOp::Div || b->op == ast::BinaryOp::Mod ||
            b->op == ast::BinaryOp::BitAnd || b->op == ast::BinaryOp::BitOr ||
            b->op == ast::BinaryOp::BitXor || b->op == ast::BinaryOp::Shl ||
            b->op == ast::BinaryOp::Shr || b->op == ast::BinaryOp::UShr ||
            b->op == ast::BinaryOp::Exp) {
            return true;
        }
        if (b->op == ast::BinaryOp::Add) {
            return isNumericIndexExpr(b->lhs.get()) || isNumericIndexExpr(b->rhs.get());
        }
    }
    return false;
}

class ReachabilityScan final : public Walker {
public:
    explicit ReachabilityScan(const LiteralNameScan& literalNames) : literalNames_(literalNames) {}

    using Walker::visit;

    std::set<std::string> memberNames;
    bool anyComputedMember = false;

    void visit(const ast::MemberAccess& n) override {
        if (!n.isPrivate && !n.property.empty()) {
            memberNames.insert(n.property);
        }
        Walker::visit(n);
    }

    void visit(const ast::SuperMember& n) override {
        if (!n.property.empty()) {
            memberNames.insert(n.property);
        }
        Walker::visit(n);
    }

    void visit(const ast::IndexAccess& n) override {
        if (!isNumericIndexExpr(n.index.get())) {
            std::set<std::string> named;
            if (possibleNames(*n.index, literalNames_, named)) {
                for (const auto& name : named) memberNames.insert(name);
            } else if (!isNonClassPropertyAccess(n.object.get(), std::string())) {
                bool isThisField = false;
                if (const auto* m = dynamic_cast<const ast::MemberAccess*>(n.object.get())) {
                    if (dynamic_cast<const ast::ThisExpr*>(m->object.get()) != nullptr) {
                        isThisField = true;
                    }
                }
                if (!isThisField) {
                    anyComputedMember = true;
                }
            }
        }
        Walker::visit(n);
    }

private:
    const LiteralNameScan& literalNames_;
};

class TopLevelObjectLitScan final : public Walker {
public:
    using Walker::visit;
    std::vector<std::pair<std::string, const ast::FunctionExpr*>> found;

    void visit(const ast::FunctionDecl&) override {}
    void visit(const ast::FunctionExpr&) override {}
    void visit(const ast::ClassDecl&) override {}
    void visit(const ast::ClassExpr&) override {}

    void visit(const ast::ObjectLit& n) override {
        for (const auto& p : n.props) {
            if (!p.keyExpr && p.accessor == ast::AccessorKind::None && !p.key.empty()) {
                if (const auto* f = dynamic_cast<const ast::FunctionExpr*>(p.value.get())) {
                    found.emplace_back(p.key, f);
                }
            }
        }
        Walker::visit(n);
    }
};

void MethodTable::build(const ast::Module& module) {
    LiteralNameScan literalNames;
    literalNames.visit(module);
    ReachabilityScan reach(literalNames);
    reach.visit(module);
    memberNames_ = std::move(reach.memberNames);
    anyComputedMember_ = reach.anyComputedMember;

    ClassScan scan;
    scan.visit(module);
    duplicates_ = std::move(scan.duplicates);
    for (const auto& cls : scan.found) {
        if (classes_.count(cls.name) != 0) continue;
        ClassNode node;
        node.superName = cls.superName;
        for (const auto& m : *cls.methods) {
            if (m.fn != nullptr && !m.isStatic && !m.isStaticBlock && !m.isField &&
                !m.isConstructor && !m.computed() && !m.name.empty() &&
                m.accessor == ast::AccessorKind::Setter) {
                SetterInfo s;
                s.fn = m.fn.get();
                s.className = cls.name;
                s.propName = m.name;
                s.plainParams = paramsArePlain(m.fn->params);
                const uint32_t sIdx = static_cast<uint32_t>(setters_.size());
                setters_.push_back(std::move(s));
                setterByNode_[m.fn.get()] = sIdx;
                continue;
            }
            if (m.fn != nullptr && !m.isStatic && !m.isStaticBlock && !m.isField &&
                !m.isConstructor && !m.computed() && !m.name.empty() &&
                m.accessor == ast::AccessorKind::Getter) {
                if (m.fn->body.size() == 1) {
                    if (const auto* ret = dynamic_cast<const ast::ReturnStmt*>(m.fn->body[0].get())) {
                        if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(ret->value.get())) {
                            if (!mem->isPrivate && !mem->optional &&
                                dynamic_cast<const ast::ThisExpr*>(mem->object.get()) != nullptr) {
                                getterBackingField_[{cls.name, m.name}] = mem->property;
                            }
                        }
                    }
                }
                continue;
            }
            if (m.fn != nullptr && m.isStatic && !m.isStaticBlock && !m.isField &&
                !m.isConstructor && !m.computed() && !m.name.empty() &&
                m.accessor == ast::AccessorKind::None) {
                SetterInfo s;
                s.fn = m.fn.get();
                s.className = cls.name;
                s.propName = m.name;
                s.isStatic = true;
                const uint32_t sIdx = static_cast<uint32_t>(setters_.size());
                setters_.push_back(std::move(s));
                setterByNode_[m.fn.get()] = sIdx;
                continue;
            }
            if (!isInstanceMethod(m)) continue;
            MethodInfo info;
            info.fn = m.fn.get();
            info.className = cls.name;
            info.methodName = m.name;
            info.plainParams = paramsArePlain(m.fn->params);
            info.reached = info.methodName == "toString" || info.methodName == "valueOf";
            const size_t paramCount = m.fn->params.size();
            info.signature.params.assign(paramCount, Type::never());
            info.observedParams.assign(paramCount, Type::never());
            info.sawSkippedDynamicArg.assign(paramCount, false);
            info.observedParamShapes.assign(paramCount, Type::never());
            info.paramShapes.assign(paramCount, Type::never());
            info.hasDefault.assign(paramCount, false);
            info.safeParamNames.assign(paramCount, std::string());
            RebindScan rebound;
            rebound.walkList(m.fn->body);
            for (size_t p = 0; p < paramCount; ++p) {
                info.hasDefault[p] = m.fn->params[p].defaultValue != nullptr;
                if (m.fn->params[p].isRest || m.fn->params[p].pattern ||
                    m.fn->params[p].name.empty()) {
                    continue;
                }
                if (rebound.names.count(m.fn->params[p].name) != 0) continue;
                info.safeParamNames[p] = m.fn->params[p].name;
            }
            const uint32_t index = static_cast<uint32_t>(methods_.size());
            methods_.push_back(std::move(info));
            byNode_[m.fn.get()] = index;
            byName_[m.name].push_back(index);
            node.ownMethods[m.name] = index;
        }
        classes_.emplace(cls.name, std::move(node));
    }
    for (auto& entry : classes_) {
        if (entry.second.superName.empty()) continue;
        const auto parent = classes_.find(entry.second.superName);
        if (parent == classes_.end()) continue;
        parent->second.children.push_back(entry.first);
    }

    TopLevelObjectLitScan objScan;
    objScan.visit(module);
    for (const auto& [propName, fn] : objScan.found) {
        if (setterByNode_.count(fn) != 0) continue;
        SetterInfo s;
        s.fn = fn;
        s.propName = propName;
        s.isStatic = true;
        s.plainParams = paramsArePlain(fn->params);
        const uint32_t sIdx = static_cast<uint32_t>(setters_.size());
        setters_.push_back(std::move(s));
        setterByNode_[fn] = sIdx;
    }
}

uint32_t MethodTable::indexOfNode(const ast::FunctionExpr* fn) const {
    const auto it = byNode_.find(fn);
    return it == byNode_.end() ? kNoMethod : it->second;
}

uint32_t MethodTable::setterIndexOfNode(const ast::FunctionExpr* fn) const {
    const auto it = setterByNode_.find(fn);
    return it == setterByNode_.end() ? kNoMethod : it->second;
}

bool MethodTable::isMethodName(const std::string& name) const {
    return byName_.find(name) != byName_.end();
}

const std::vector<uint32_t>* MethodTable::declarationsOf(const std::string& name) const {
    const auto it = byName_.find(name);
    return it == byName_.end() ? nullptr : &it->second;
}

void MethodTable::subtreeOf(const std::string& className, std::vector<uint32_t>& out) const {
    std::vector<const std::string*> stack{&className};
    while (!stack.empty()) {
        const std::string* name = stack.back();
        stack.pop_back();
        const auto it = classes_.find(*name);
        if (it == classes_.end()) continue;
        for (const auto& entry : it->second.ownMethods) {
            out.push_back(entry.second);
        }
        for (const auto& child : it->second.children) stack.push_back(&child);
    }
}

void MethodTable::ancestorsOf(const std::string& className, std::vector<std::string>& out) const {
    const auto it = classes_.find(className);
    if (it == classes_.end()) return;
    for (const ClassNode* node = &it->second; !node->superName.empty();) {
        out.push_back(node->superName);
        const auto up = classes_.find(node->superName);
        if (up == classes_.end()) break;
        node = &up->second;
    }
}

bool MethodTable::areClassesRelated(const std::string& a, const std::string& b) const {
    if (a.empty() || b.empty()) return true;
    if (a == b) return true;
    std::vector<std::string> ancA;
    ancestorsOf(a, ancA);
    if (std::find(ancA.begin(), ancA.end(), b) != ancA.end()) return true;
    std::vector<std::string> ancB;
    ancestorsOf(b, ancB);
    return std::find(ancB.begin(), ancB.end(), a) != ancB.end();
}

bool MethodTable::isStaticMethodReachable(const std::string& /*className*/,
                                          const std::string& methodName) const {
    return memberNames_.count(methodName) != 0;
}

std::string MethodTable::getterBackingField(const std::string& className,
                                            const std::string& propName) const {
    for (std::string cur = className; !cur.empty();) {
        const auto it = getterBackingField_.find({cur, propName});
        if (it != getterBackingField_.end()) return it->second;
        const auto cIt = classes_.find(cur);
        if (cIt == classes_.end()) break;
        cur = cIt->second.superName;
    }
    return std::string();
}

void MethodTable::reachableFrom(const std::string& className, const std::string& methodName,
                                std::vector<uint32_t>& out) const {
    const auto start = classes_.find(className);
    if (start == classes_.end()) return;

    // Up: nearest declaration at or above this class
    for (const ClassNode* node = &start->second;;) {
        const auto own = node->ownMethods.find(methodName);
        if (own != node->ownMethods.end()) {
            out.push_back(own->second);
            break;
        }
        if (node->superName.empty()) break;
        const auto up = classes_.find(node->superName);
        if (up == classes_.end()) break;
        node = &up->second;
    }

    // Down: every override in subtree
    std::vector<const std::string*> stack{&start->first};
    while (!stack.empty()) {
        const std::string* name = stack.back();
        stack.pop_back();
        const auto it = classes_.find(*name);
        if (it == classes_.end()) continue;
        const auto own = it->second.ownMethods.find(methodName);
        if (own != it->second.ownMethods.end() &&
            std::find(out.begin(), out.end(), own->second) == out.end()) {
            out.push_back(own->second);
        }
        for (const auto& child : it->second.children) stack.push_back(&child);
    }
}

std::map<std::string, std::map<std::string, std::map<std::string, Type>>> MethodTable::harvestOracle() const {
    std::map<std::string, std::map<std::string, std::map<std::string, Type>>> out;
    for (uint32_t i = 0; i < methods_.size(); ++i) {
        const auto& m = methods_[i];
        if (!m.reached) continue;
        std::map<std::string, Type> params;
        for (size_t p = 0; p < m.safeParamNames.size(); ++p) {
            if (m.safeParamNames[p].empty()) continue;
            if (p >= m.signature.params.size()) continue;
            params.emplace(m.safeParamNames[p], m.signature.params[p]);
        }
        if (!params.empty()) {
            out[m.className].emplace(m.methodName, std::move(params));
        }
    }
    return out;
}

void scanMethodEscapes(const ast::Module& module, const MethodTable& table, MethodPoison& out) {
    LiteralNameScan names;
    names.visit(module);
    EscapeScan scan(table, names, out);
    scan.visit(module);
}

}  // namespace bronze::types
