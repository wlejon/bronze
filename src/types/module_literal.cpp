#include "types/module_literal.h"

#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "types/walk.h"

namespace bronze::types {

namespace {

// The two members that redefine a property of the object they are called ON.
// Every other redefiner in the language — `Object.defineProperty`,
// `Object.defineProperties`, `Reflect.defineProperty` — takes its target as an
// argument, so handing the object over is what reaches it, and the escape rule
// already refuses that. These two do not, so they are named.
bool redefinesThroughReceiver(const std::string& member) {
    return member == "__defineGetter__" || member == "__defineSetter__";
}

// Counts every place a name is bound or rebound: a declaration, a parameter, a
// catch parameter, a for-in/of head, a function or class name, an assignment,
// an increment. A candidate binding must come out of this at exactly one — its
// own declaration — and that single count is what makes "the identifier at the
// read site" and "the object the literal built" the same thing without a scope
// walk. Deliberately name-based, and every extra count refuses a claim rather
// than licensing one.
class BindingCounts final : public Walker {
public:
    using Walker::visit;

    std::map<std::string, uint32_t> counts;

    void visit(const ast::VarDecl& n) override {
        bind(n.name);
        bindPattern(n.pattern.get());
        Walker::visit(n);
    }
    void visit(const ast::FunctionDecl& n) override {
        bind(n.name);
        bindParams(n.params);
        Walker::visit(n);
    }
    void visit(const ast::FunctionExpr& n) override {
        bind(n.name);
        bindParams(n.params);
        Walker::visit(n);
    }
    void visit(const ast::ClassDecl& n) override {
        bind(n.name);
        Walker::visit(n);
    }
    void visit(const ast::ClassExpr& n) override {
        bind(n.name);
        Walker::visit(n);
    }
    void visit(const ast::TryStmt& n) override {
        if (n.hasCatchParam) {
            bind(n.catchName);
            bindPattern(n.catchPattern.get());
        }
        Walker::visit(n);
    }
    void visit(const ast::ForInStmt& n) override {
        bind(n.name);
        bindPattern(n.pattern.get());
        Walker::visit(n);
    }
    void visit(const ast::ForOfStmt& n) override {
        bind(n.name);
        bindPattern(n.pattern.get());
        Walker::visit(n);
    }
    void visit(const ast::DestructuringAssign& n) override {
        for (const auto& name : ast::patternBoundNames(*n.pattern)) bind(name);
        Walker::visit(n);
    }
    void visit(const ast::Binary& n) override {
        if (ast::isAssignOp(n.op)) {
            if (const auto* id = dynamic_cast<const ast::Ident*>(n.lhs.get())) bind(id->name);
        }
        Walker::visit(n);
    }
    void visit(const ast::Unary& n) override {
        switch (n.op) {
            case ast::UnaryOp::PreInc:
            case ast::UnaryOp::PreDec:
            case ast::UnaryOp::PostInc:
            case ast::UnaryOp::PostDec:
                if (const auto* id = dynamic_cast<const ast::Ident*>(n.operand.get())) bind(id->name);
                break;
            default:
                break;
        }
        Walker::visit(n);
    }

private:
    void bind(const std::string& name) {
        if (!name.empty()) ++counts[name];
    }
    void bindPattern(const ast::BindingPattern* pattern) {
        if (pattern == nullptr) return;
        for (const auto& name : ast::patternBoundNames(*pattern)) bind(name);
    }
    void bindParams(const std::vector<ast::Param>& params) {
        for (const auto& p : params) {
            bind(p.name);
            bindPattern(p.pattern.get());
        }
    }
};

// Where a name — and, on a second run over one literal's own functions, `this`
// — is allowed to appear.
//
// `X.name` and `X["name"]` name a property of the object; a call `X.m()` is the
// same position and additionally passes the object as a receiver, which is what
// the `this` half of this scan is for. Every OTHER position hands the object
// itself to code this pass cannot follow, so the name is marked escaped and no
// claim is made about it.
class ReferenceScan final : public Walker {
public:
    using Walker::visit;

    std::set<std::string> escaped;
    std::set<std::string> receiverRedefined;
    std::set<std::string> deletedKeys;
    // `X.m(...)` — the one construct that makes `this` be `X` without `X` ever
    // being a value, so the member it names has to be a function this pass can
    // read, and nothing may have put a different one there.
    std::map<std::string, std::set<std::string>> calledMembers;
    std::map<std::string, std::set<std::string>> assignedMembers;
    std::set<std::string> thisAssignedMembers;
    bool thisEscaped = false;
    bool sawEval = false;

    void visit(const ast::Ident& n) override {
        if (n.name == "eval") sawEval = true;
        escaped.insert(n.name);
    }
    void visit(const ast::ThisExpr&) override { thisEscaped = true; }

    void visit(const ast::MemberAccess& n) override {
        if (!n.isPrivate && exemptReceiver(*n.object, n.property)) return;
        Walker::visit(n);
    }

    void visit(const ast::IndexAccess& n) override {
        const auto* key = dynamic_cast<const ast::StringLit*>(n.index.get());
        if (key != nullptr && exemptReceiver(*n.object, key->value)) return;
        Walker::visit(n);
    }

    void visit(const ast::Call& n) override {
        noteReceiverCall(*n.callee);
        Walker::visit(n);
    }
    void visit(const ast::TaggedTemplate& n) override {
        noteReceiverCall(*n.tag);
        Walker::visit(n);
    }

    void visit(const ast::Binary& n) override {
        if (ast::isAssignOp(n.op)) noteAssignTarget(*n.lhs);
        Walker::visit(n);
    }

    void visit(const ast::DestructuringAssign& n) override {
        noteAssignPattern(*n.pattern);
        Walker::visit(n);
    }

    void visit(const ast::Unary& n) override {
        switch (n.op) {
            case ast::UnaryOp::Delete:
                noteDelete(*n.operand);
                break;
            case ast::UnaryOp::PreInc:
            case ast::UnaryOp::PreDec:
            case ast::UnaryOp::PostInc:
            case ast::UnaryOp::PostDec:
                noteAssignTarget(*n.operand);
                break;
            default:
                break;
        }
        Walker::visit(n);
    }

private:
    // The receiver of a named member access, consumed rather than walked. The
    // return value says the mention was accounted for here; walking it would
    // count it as the object escaping.
    bool exemptReceiver(const ast::Expr& object, const std::string& member) {
        if (const auto* id = dynamic_cast<const ast::Ident*>(&object)) {
            if (id->name == "eval") sawEval = true;
            if (redefinesThroughReceiver(member)) receiverRedefined.insert(id->name);
            return true;
        }
        if (dynamic_cast<const ast::ThisExpr*>(&object) != nullptr) {
            if (redefinesThroughReceiver(member)) thisEscaped = true;
            return true;
        }
        return false;
    }

    // The receiver and member of a call written as a member access, which is
    // the form that makes the receiver the callee's `this`. A callee reached
    // through a computed key walks its receiver like any other expression, so
    // the receiver escapes there and needs no entry.
    void noteReceiverCall(const ast::Expr& callee) {
        const ast::Expr* object = nullptr;
        const std::string* member = nullptr;
        if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(&callee)) {
            if (mem->isPrivate) return;
            object = mem->object.get();
            member = &mem->property;
        } else if (const auto* idx = dynamic_cast<const ast::IndexAccess*>(&callee)) {
            const auto* key = dynamic_cast<const ast::StringLit*>(idx->index.get());
            if (key == nullptr) return;
            object = idx->object.get();
            member = &key->value;
        } else {
            return;
        }
        if (const auto* id = dynamic_cast<const ast::Ident*>(object)) {
            calledMembers[id->name].insert(*member);
        }
    }

    // Which members a write can replace. A member that is both written and
    // called is a member whose function this pass has not read, so a receiver
    // with one of those hands `this` to code it cannot follow.
    void noteAssignTarget(const ast::Expr& target) {
        const ast::Expr* object = nullptr;
        const std::string* member = nullptr;
        if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(&target)) {
            if (mem->isPrivate) return;
            object = mem->object.get();
            member = &mem->property;
        } else if (const auto* idx = dynamic_cast<const ast::IndexAccess*>(&target)) {
            const auto* key = dynamic_cast<const ast::StringLit*>(idx->index.get());
            if (key == nullptr) return;
            object = idx->object.get();
            member = &key->value;
        } else {
            return;
        }
        if (const auto* id = dynamic_cast<const ast::Ident*>(object)) {
            assignedMembers[id->name].insert(*member);
            return;
        }
        if (dynamic_cast<const ast::ThisExpr*>(object) != nullptr) {
            thisAssignedMembers.insert(*member);
        }
    }

    void noteAssignPattern(const ast::BindingPattern& pattern) {
        for (const auto& elem : pattern.elements) {
            if (elem.target) noteAssignTarget(*elem.target);
            if (elem.pattern) noteAssignPattern(*elem.pattern);
        }
    }

    void noteDelete(const ast::Expr& target) {
        if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(&target)) {
            if (!mem->isPrivate) deletedKeys.insert(mem->property);
            return;
        }
        // A computed key deletes from whatever the receiver holds. A binding
        // that could BE that receiver has already handed itself over to reach
        // this position, so it is escaped and no claim about it survives.
        if (const auto* idx = dynamic_cast<const ast::IndexAccess*>(&target)) {
            if (const auto* s = dynamic_cast<const ast::StringLit*>(idx->index.get())) {
                deletedKeys.insert(s->value);
            }
        }
    }
};

// What one key of the literal finally is, after every definition of that key
// has run. ECMA-262 13.2.5.5 evaluates a literal's PropertyDefinitions in
// order, so a later one replaces an earlier one — a data property replaces an
// accessor exactly as an accessor replaces a data property — and the two halves
// of one accessor combine rather than replace.
struct Entry {
    const ast::FunctionExpr* getter = nullptr;
    const ast::Expr* value = nullptr;  // the data property's initializer
    bool data = false;
};

// The literal's final key table, or false when the literal has a shape this
// pass will not reason about at all: a computed key (which can name any of the
// others), a spread (which brings in keys from a value), `__proto__` (which is
// the prototype and not a property), or a cover-initialized name (which is not
// a literal at all).
bool literalEntries(const ast::ObjectLit& lit, std::map<std::string, Entry>& out) {
    for (const auto& p : lit.props) {
        if (p.computed() || p.coverInitialized) return false;
        if (dynamic_cast<const ast::SpreadElement*>(p.value.get()) != nullptr) return false;
        if (p.key == "__proto__") return false;
        Entry& e = out[p.key];
        switch (p.accessor) {
            case ast::AccessorKind::Getter:
                e.data = false;
                e.value = nullptr;
                e.getter = dynamic_cast<const ast::FunctionExpr*>(p.value.get());
                break;
            case ast::AccessorKind::Setter:
                // A setter makes the property an accessor and leaves whatever
                // getter half is already there in place.
                e.data = false;
                e.value = nullptr;
                break;
            case ast::AccessorKind::None:
                e.data = true;
                e.value = p.value.get();
                e.getter = nullptr;
                break;
        }
    }
    return true;
}

// The key a getter of the exact form `get p() { return this.q; }` reads, when
// `q` is a data property this same literal defines — so the forwarded read hits
// an own slot of the object rather than starting a prototype walk somewhere
// else. Anything with a second statement, a parameter, a directive prologue, or
// a computed or private target is not this form and gets no answer.
const std::string* forwardTarget(const ast::FunctionExpr& fn,
                                 const std::map<std::string, Entry>& entries) {
    if (fn.kind != ast::FunctionKind::Accessor) return nullptr;
    if (!fn.params.empty() || fn.isArrow || fn.isAsync || fn.isGenerator) return nullptr;
    if (fn.body.size() != 1) return nullptr;
    const auto* ret = dynamic_cast<const ast::ReturnStmt*>(fn.body.front().get());
    if (ret == nullptr || ret->value == nullptr) return nullptr;
    const auto* mem = dynamic_cast<const ast::MemberAccess*>(ret->value.get());
    if (mem == nullptr || mem->optional || mem->isPrivate) return nullptr;
    if (dynamic_cast<const ast::ThisExpr*>(mem->object.get()) == nullptr) return nullptr;
    const auto it = entries.find(mem->property);
    if (it == entries.end() || !it->second.data) return nullptr;
    return &it->first;
}

const std::set<std::string>& calledOn(const ReferenceScan& refs, const std::string& binding) {
    static const std::set<std::string> none;
    const auto it = refs.calledMembers.find(binding);
    return it == refs.calledMembers.end() ? none : it->second;
}

bool disjoint(const std::set<std::string>& a, const std::set<std::string>& b) {
    for (const auto& name : a) {
        if (b.count(name) != 0) return false;
    }
    return true;
}

bool calledMembersAreOwnFunctions(const ReferenceScan& refs, const std::string& binding,
                                  const std::map<std::string, Entry>& entries) {
    const auto assigned = refs.assignedMembers.find(binding);
    for (const auto& member : calledOn(refs, binding)) {
        if (assigned != refs.assignedMembers.end() && assigned->second.count(member) != 0) {
            return false;
        }
        const auto e = entries.find(member);
        // An ACCESSOR here would be called too: `X.p()` evaluates the reference
        // and then calls what it answered with `X` as the receiver, so the
        // getter's result gets `this` just as a method would.
        if (e == entries.end() || !e->second.data) return false;
        if (dynamic_cast<const ast::FunctionExpr*>(e->second.value) == nullptr) return false;
    }
    return true;
}

}  // namespace

bool moduleLiteralDevirtDisabled() {
    return std::getenv("BRONZE_NO_MODULE_LITERAL_DEVIRT") != nullptr;
}

void ModuleLiteralAccessors::scan(const ast::Module& module) {
    if (moduleLiteralDevirtDisabled()) return;

    ReferenceScan refs;
    module.accept(refs);
    // Compiled ahead of time, bronze refuses `eval` outright; the check is here
    // so that this proof does not silently depend on that happening first.
    if (refs.sawEval) return;

    BindingCounts counts;
    module.accept(counts);

    for (const auto& stmt : module.body) {
        const auto* decl = dynamic_cast<const ast::VarDecl*>(stmt.get());
        if (decl == nullptr || !decl->isConst || decl->name.empty() || decl->pattern) continue;
        const auto* lit = dynamic_cast<const ast::ObjectLit*>(decl->init.get());
        // A module namespace object's properties are exotic: their [[Set]]
        // always refuses and their descriptors are the linker's, not this
        // literal's, so the equality this table claims is not the one holding.
        if (lit == nullptr || lit->isModuleNamespace) continue;

        const auto seen = counts.counts.find(decl->name);
        if (seen == counts.counts.end() || seen->second != 1) continue;
        if (refs.escaped.count(decl->name) != 0) continue;
        if (refs.receiverRedefined.count(decl->name) != 0) continue;

        std::map<std::string, Entry> entries;
        if (!literalEntries(*lit, entries)) continue;

        // A call `X.m(...)` makes `this` be `X` inside `m` without `X` ever
        // having been a value, so the escape rule above cannot see it. Every
        // member the program calls on this binding therefore has to be a
        // function written INSIDE the literal — one this pass reads below —
        // and nothing may have replaced it: a member that is both called and
        // assigned is a call into whatever the assignment put there.
        if (!calledMembersAreOwnFunctions(refs, decl->name, entries)) continue;

        // `this` inside the literal's own functions IS this object, so a
        // mention of it anywhere but as a receiver hands the object on.
        ReferenceScan inner;
        for (const auto& p : lit->props) {
            if (const auto* fn = dynamic_cast<const ast::FunctionExpr*>(p.value.get())) {
                fn->accept(inner);
            }
        }
        if (inner.thisEscaped) continue;
        if (!disjoint(calledOn(refs, decl->name), inner.thisAssignedMembers)) continue;

        for (const auto& [key, entry] : entries) {
            if (entry.getter == nullptr) continue;
            const std::string* backing = forwardTarget(*entry.getter, entries);
            if (backing == nullptr) continue;
            // `get p() { return this.p; }` is unbounded recursion, and the
            // forwarded read would answer where the program overflows.
            if (*backing == key) continue;
            if (refs.deletedKeys.count(key) != 0) continue;
            forward_[decl->name][key] = *backing;
        }
    }
}

const std::string* ModuleLiteralAccessors::backingKey(const std::string& binding,
                                                      const std::string& name) const {
    const auto b = forward_.find(binding);
    if (b == forward_.end()) return nullptr;
    const auto p = b->second.find(name);
    if (p == b->second.end()) return nullptr;
    return &p->second;
}

std::vector<std::string> ModuleLiteralAccessors::report() const {
    std::vector<std::string> lines;
    for (const auto& [binding, props] : forward_) {
        for (const auto& [name, backing] : props) {
            lines.push_back(binding + "." + name + " -> " + backing);
        }
    }
    return lines;
}

}  // namespace bronze::types
