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
            return;
        }
        if (const auto* m = dynamic_cast<const ast::MemberAccess*>(n.callee.get())) {
            if (m->property == "call" || m->property == "apply" || m->property == "bind") {
                if (const auto* inner = dynamic_cast<const ast::MemberAccess*>(m->object.get())) {
                    out_.addDeclarations(table_, inner->property, "reached through .call/.apply/.bind");
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

    void visit(const ast::MemberAccess& n) override {
        if (!consumed_) {
            if (table_.isMethodName(n.property)) {
                out_.addDeclarations(table_, n.property, "read as a value rather than called");
            }
        }
        consume(*n.object);
    }

    void visit(const ast::SuperMember& n) override {
        if (!consumed_) {
            if (table_.isMethodName(n.property)) {
                out_.addDeclarations(table_, n.property, "read as a value rather than called");
            }
        }
        if (n.baseExpr) consume(*n.baseExpr);
    }

    void visit(const ast::Unary& n) override { consume(*n.operand); }

    void visit(const ast::IndexAccess& n) override {
        if (!consumed_) {
            std::set<std::string> named;
            if (possibleNames(*n.index, names_, named)) {
                for (const auto& name : named) {
                    if (table_.isMethodName(name)) {
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

void MethodTable::build(const ast::Module& module) {
    ClassScan scan;
    scan.visit(module);
    duplicates_ = std::move(scan.duplicates);
    for (const auto& cls : scan.found) {
        if (classes_.count(cls.name) != 0) continue;
        ClassNode node;
        node.superName = cls.superName;
        for (const auto& m : *cls.methods) {
            if (!isInstanceMethod(m)) continue;
            MethodInfo info;
            info.fn = m.fn.get();
            info.className = cls.name;
            info.methodName = m.name;
            info.plainParams = paramsArePlain(m.fn->params);
            const size_t paramCount = m.fn->params.size();
            info.signature.params.assign(paramCount, Type::never());
            info.observedParams.assign(paramCount, Type::never());
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
}

uint32_t MethodTable::indexOfNode(const ast::FunctionExpr* fn) const {
    const auto it = byNode_.find(fn);
    return it == byNode_.end() ? kNoMethod : it->second;
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
