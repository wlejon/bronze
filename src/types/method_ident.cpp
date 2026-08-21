#include "types/method_ident.h"

#include <algorithm>

#include "types/walk.h"

namespace bronze::types {
namespace {

// Whether a class member is an ordinary instance method: something a call
// `recv.name(...)` can reach, whose `this` is an instance.
//
// A static method is excluded because its receiver is the CONSTRUCTOR, whose
// class this analysis does not model; a field is not a callable at all; a
// computed key has no name until it is evaluated; and a constructor is reached
// by `new`, which this pass deliberately leaves alone (see method_ident.h).
//
// An ACCESSOR is excluded for a sharper reason. `get alphaTest()` is invoked by
// READING `material.alphaTest`, which is the same syntax as reading a field, and
// `set alphaTest(v)` takes its argument from an ASSIGNMENT. Neither is a call
// site, so the join has nothing to join — and, worse, treating one as a method
// makes every read of the name look like a method escaping as a value. Three.js
// measured that: `alphaTest`, `anisotropy`, `clearcoat`, `encoding`, `count`,
// `center`, `height` and thirty more were poisoned as escaped methods when the
// program had only ever read a property.
bool isInstanceMethod(const ast::ClassMethod& m) {
    return m.fn != nullptr && !m.isStatic && !m.isStaticBlock && !m.isField &&
           !m.isConstructor && !m.computed() && !m.name.empty() &&
           m.accessor == ast::AccessorKind::None;
}

bool paramsArePlain(const std::vector<ast::Param>& params) {
    for (const auto& p : params) {
        if (p.defaultValue || p.isRest || p.pattern) return false;
    }
    return true;
}

// Collects every `class` that has a name to resolve `extends` against, which is
// the same set `ClassLayoutTable` collects — the two have to agree, because a
// receiver's class arrives here as a name that table produced.
class ClassScan final : public Walker {
public:
    using Walker::visit;

    struct Found {
        std::string name;
        std::string superName;
        const std::vector<ast::ClassMethod>* methods = nullptr;
    };
    std::vector<Found> found;

    void visit(const ast::ClassDecl& n) override {
        if (!n.name.empty()) found.push_back({n.name, n.superName, &n.methods});
        Walker::visit(n);
    }
    void visit(const ast::ClassExpr& n) override {
        if (!n.name.empty()) found.push_back({n.name, n.superName, &n.methods});
        Walker::visit(n);
    }
};

// Which string literals a name can hold, over the WHOLE program.
//
// Only used to pin the property a computed call names. Module-wide rather than
// per-function on purpose: two functions with a local of the same name merge
// into one entry, which can only make the answer a SUPERSET of what either
// site could name — and a superset is the safe direction for a poison list.
class LiteralNameScan final : public Walker {
public:
    using Walker::visit;

    std::map<std::string, std::set<std::string>> literals;
    std::set<std::string> unknown;

    void visit(const ast::VarDecl& n) override {
        if (!n.name.empty()) note(n.name, n.init.get());
        if (n.pattern) {
            for (const auto& name : ast::patternBoundNames(*n.pattern)) unknown.insert(name);
        }
        Walker::visit(n);
    }
    void visit(const ast::Binary& n) override {
        if (ast::isAssignOp(n.op)) {
            if (const auto* id = dynamic_cast<const ast::Ident*>(n.lhs.get())) {
                note(id->name, n.op == ast::BinaryOp::Assign ? n.rhs.get() : nullptr);
            }
        }
        Walker::visit(n);
    }
    void visit(const ast::DestructuringAssign& n) override {
        for (const auto& name : ast::patternBoundNames(*n.pattern)) unknown.insert(name);
        Walker::visit(n);
    }
    // A parameter is bound by the caller, so nothing here can bound it. Both
    // function forms go through the same two lists.
    void visit(const ast::FunctionExpr& n) override {
        noteParams(n.params);
        Walker::visit(n);
    }
    void visit(const ast::FunctionDecl& n) override {
        noteParams(n.params);
        Walker::visit(n);
    }
    void visit(const ast::ForInStmt& n) override {
        if (!n.name.empty()) unknown.insert(n.name);
        Walker::visit(n);
    }
    void visit(const ast::ForOfStmt& n) override {
        if (!n.name.empty()) unknown.insert(n.name);
        Walker::visit(n);
    }

private:
    void noteParams(const std::vector<ast::Param>& params) {
        for (const auto& p : params) {
            if (!p.name.empty()) unknown.insert(p.name);
            if (p.pattern) {
                for (const auto& b : ast::patternBoundNames(*p.pattern)) unknown.insert(b);
            }
        }
    }
    void note(const std::string& name, const ast::Expr* init) {
        // A declaration with no initialiser stores `undefined`, which is not a
        // property name and cannot widen the set.
        if (init == nullptr) {
            if (name.empty()) return;
            return;
        }
        if (const auto* s = dynamic_cast<const ast::StringLit*>(init)) {
            literals[name].insert(s->value);
            return;
        }
        unknown.insert(name);
    }
};

// The property names a computed call `o[ index ](...)` can name, when they are
// a finite set the source spells out. `false` means "any name at all".
bool possibleNames(const ast::Expr& index, const LiteralNameScan& env,
                   std::set<std::string>& out) {
    if (const auto* s = dynamic_cast<const ast::StringLit*>(&index)) {
        out.insert(s->value);
        return true;
    }
    if (const auto* t = dynamic_cast<const ast::Ternary*>(&index)) {
        return possibleNames(*t->thenExpr, env, out) && possibleNames(*t->elseExpr, env, out);
    }
    if (const auto* id = dynamic_cast<const ast::Ident*>(&index)) {
        if (env.unknown.count(id->name) != 0) return false;
        const auto it = env.literals.find(id->name);
        if (it == env.literals.end()) return false;
        out.insert(it->second.begin(), it->second.end());
        return true;
    }
    return false;
}

// Everything decidable from the program text alone. See the declaration.
class EscapeScan final : public Walker {
public:
    EscapeScan(const MethodTable& table, const LiteralNameScan& names, MethodPoison& out)
        : table_(table), names_(names), out_(out) {}

    using Walker::visit;

    void visit(const ast::Call& n) override {
        // An argument is a value handed to someone else, so a method name in one
        // IS escaping, and the flag has to be off for the walk.
        const bool saved = consumed_;
        consumed_ = false;
        for (const auto& a : n.args) a->accept(*this);
        consumed_ = saved;

        // `super.m(...)`: a call, and the most common one in three.js — every
        // `copy`, `clone`, `dispose`, `toJSON` and `updateMatrixWorld` chains
        // through one. The name is in callee position exactly as an ordinary
        // method call's is, and reading it as an escape poisoned thirty of the
        // library's workhorse method names on evidence that was a call.
        if (const auto* sup = dynamic_cast<const ast::SuperMember*>(n.callee.get())) {
            if (sup->baseExpr) consume(*sup->baseExpr);
            return;
        }
        if (const auto* m = dynamic_cast<const ast::MemberAccess*>(n.callee.get())) {
            // `f.call(x, ...)`, `f.apply(x, a)`, `f.bind(x)`: the receiver the
            // call runs on is the ARGUMENT, not `f`'s owner, so the class this
            // analysis would have reasoned from is not the one that arrives.
            // Whatever method `f` names gives up its parameters.
            if (m->property == "call" || m->property == "apply" || m->property == "bind") {
                if (const auto* inner =
                        dynamic_cast<const ast::MemberAccess*>(m->object.get())) {
                    poison(inner->property, "reached through .call/.apply/.bind");
                    inner->object->accept(*this);
                    return;
                }
                // `f.call(...)` on something that is not a member read: the
                // callee is a function VALUE, and the name it came from was
                // already poisoned where it was read.
                m->object->accept(*this);
                return;
            }
            // An ordinary method call. The name is in callee position, so it is
            // not escaping here; whether the RECEIVER is one this analysis can
            // name is a question about types, and the flow pass answers it.
            m->object->accept(*this);
            return;
        }
        if (const auto* ix = dynamic_cast<const ast::IndexAccess*>(n.callee.get())) {
            ix->object->accept(*this);
            ix->index->accept(*this);
            std::set<std::string> named;
            if (possibleNames(*ix->index, names_, named)) {
                for (const auto& name : named) {
                    poison(name, "reached through a computed call");
                }
            } else {
                out_.addAll("a computed call whose property name is not a fixed set");
            }
            return;
        }
        n.callee->accept(*this);
    }

    // A method name read anywhere but in callee position is a function value
    // going somewhere this analysis cannot follow: a callback, a listener, a
    // field on another object, an argument to host code. Whoever calls it
    // passes what they like.
    //
    // Unless the read is CONSUMED where it stands. `i < array.length` produces a
    // number and stores nothing; `box.min.x` indexes into the value and stores
    // nothing; `!material.onBeforeCompile` tests it and stores nothing. None of
    // those can hand a function to a caller, and treating them as escapes is not
    // conservatism but noise — `length`, `min`, `max`, `center`, `scale` and
    // `distance` are method names on three.js's math classes and property names
    // on half the objects in the library, and the collision is what was being
    // reported.
    void visit(const ast::MemberAccess& n) override {
        if (!consumed_) poison(n.property, "read as a value rather than called");
        consume(*n.object);
    }
    void visit(const ast::SuperMember& n) override {
        if (!consumed_) poison(n.property, "read as a value rather than called");
        if (n.baseExpr) consume(*n.baseExpr);
    }

    // The consuming positions: an operand of an operator that produces a new
    // value, the base of a member or element read, a condition. Everything else
    // — an assignment's right-hand side, an argument, a `return`, an element of
    // a literal, a `yield`, a spread — can put the value somewhere a caller
    // reaches, and is left alone so that a read in it escapes.
    //
    // `&&`, `||` and `??` are deliberately NOT here: their value IS one of the
    // operands, so `const f = o.m || fallback` stores `o.m`.
    void visit(const ast::Unary& n) override { consume(*n.operand); }
    void visit(const ast::Binary& n) override {
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
    void visit(const ast::IndexAccess& n) override {
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

    void poison(const std::string& name, const char* reason) {
        if (!table_.isMethodName(name)) return;
        out_.add(name, reason);
    }

    const MethodTable& table_;
    const LiteralNameScan& names_;
    MethodPoison& out_;
    // Whether the expression now being walked sits in a position that CONSUMES
    // its value rather than storing or passing it. Applies to the immediate
    // expression only, which is why every consuming position sets it around one
    // child and every other position clears it.
    bool consumed_ = false;
};

const std::string kNoReason;

}  // namespace

void MethodPoison::add(const std::string& name, const std::string& reason) {
    byName.emplace(name, reason);
}

void MethodPoison::addAll(const std::string& reason) {
    if (all) return;
    all = true;
    allReason = reason;
}

const std::string& MethodPoison::reasonFor(const std::string& name) const {
    const auto it = byName.find(name);
    if (it != byName.end()) return it->second;
    if (all) return allReason;
    return kNoReason;
}

void MethodTable::build(const ast::Module& module) {
    ClassScan scan;
    scan.visit(module);
    for (const auto& cls : scan.found) {
        // A duplicate class name is a redeclaration the parser rejects at module
        // scope; a nested one that shadows is rare and the first wins, which
        // matches how ClassLayoutTable resolves `extends`.
        if (classes_.count(cls.name) != 0) continue;
        ClassNode node;
        node.superName = cls.superName;
        for (const auto& m : *cls.methods) {
            if (!isInstanceMethod(m)) continue;
            // A class body cannot declare the same method name twice in a way
            // that matters: the later definition wins at run time, so it is the
            // one to speak for.
            MethodInfo info;
            info.fn = m.fn.get();
            info.className = cls.name;
            info.methodName = m.name;
            info.plainParams = paramsArePlain(m.fn->params);
            info.signature.params.assign(m.fn->params.size(), Type::never());
            info.observedParams.assign(m.fn->params.size(), Type::never());
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

void MethodTable::reachableFrom(const std::string& className, const std::string& methodName,
                                std::vector<uint32_t>& out) const {
    const auto start = classes_.find(className);
    if (start == classes_.end()) return;

    // Up: the declaration a receiver of exactly this class would find.
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

    // Down: every override the receiver could actually have been.
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

void scanMethodEscapes(const ast::Module& module, const MethodTable& table, MethodPoison& out) {
    LiteralNameScan names;
    names.visit(module);
    EscapeScan scan(table, names, out);
    scan.visit(module);
}

}  // namespace bronze::types
