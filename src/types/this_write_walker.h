#pragma once

// ThisWriteWalker: the `this.<name> = ...` installs of one constructor or
// method body, read by class_layout.cpp (the proven layout) and
// class_capacity.cpp (the inline capacity of an unproven one).

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "types/walk.h"

namespace bronze::types::detail {

// The `this.<name> = ...` assignments one constructor or method body performs,
// in source order.
//
// It does NOT descend into nested functions: `this` inside a non-arrow function
// expression is a different receiver entirely, so an assignment there says
// nothing about this class. Arrow functions DO share `this`, and are walked —
// three.js writes `this._onChangeCallback` from arrows.
//
// An assignment that introduces a name for the first time under a BRANCH is
// recorded and also flagged: see `sawConditionalNewField` for why the class is
// then refused rather than given the optimistic order. The condition of an `if`
// and the head of a `for` are counted as branched along with their bodies —
// over-strict, and free, because a `this.x = ...` written in a loop condition
// is not a thing this analysis needs to be precise about.
class ThisWriteWalker final : public Walker {
public:
    const std::vector<ast::ClassMethod>* methods = nullptr;
    std::set<std::string> inlining;
    std::vector<std::string> names;
    // Parallel to `names`: whether that install makes a WRITABLE data property.
    // False only for a `defineProperty` descriptor that does not say
    // `writable: true`, which is the default the spec gives an unstated
    // attribute and the one three.js's `id` relies on.
    std::vector<bool> writable;
    // Late fields assigned under branches in the constructor/method
    std::vector<std::string> lateFields;
    // Accessor properties defined via Object.defineProperty(this, ...)
    std::vector<std::string> accessorNames;
    // A write this walker could not turn into a name — `this[k] = v`, or a
    // spread/destructuring target. The layout after one of these is unknown,
    // so it refuses the class rather than guessing a shorter one.
    bool sawUnmodellableWrite = false;
    // `Object.defineProperty(this, ...)` / `Object.defineProperties(this, ...)`
    // and friends: they install properties with attributes and in an order this
    // walker does not model, and the runtime may answer them by dropping the
    // object into dictionary mode. Either way the slot numbers after such a
    // call are not the ones counted here.
    bool sawThisReflection = false;
    // A name introduced for the first time under a branch that violates layout.
    bool sawConditionalNewField = false;
    // `this` used as a VALUE — `const _this = this`, `f(this)`, `[this]` —
    // rather than as the object of a member access or a method call. Once it
    // has escaped, any call may install fields through the alias
    // (WebGLRenderer's initGLContext writes `_this.capabilities` and six more
    // between `toneMappingExposure` and `xr`), so `openAt` is how many names
    // were installed at the first call after the escape: the order is known
    // up to there and not past it.
    bool thisEscaped = false;
    size_t openAt = SIZE_MAX;

    void visit(const ast::ThisExpr&) override { thisEscaped = true; }
    // `return this` (a chained setter inlined from the constructor) hands
    // `this` back to its caller, which already has it; a comparison reads it.
    // The stamp checks the prefix at run time, so what is missed here costs a
    // stamp, never a wrong slot.
    void visit(const ast::ReturnStmt& n) override {
        if (n.value && !isThis(n.value.get())) n.value->accept(*this);
    }
    void visit(const ast::MemberAccess& n) override {
        if (!isThis(n.object.get())) n.object->accept(*this);
    }
    void visit(const ast::IndexAccess& n) override {
        if (!isThis(n.object.get())) n.object->accept(*this);
        n.index->accept(*this);
    }
    void visit(const ast::NewExpr& n) override {
        Walker::visit(n);
        noteCallAfterEscape();
    }
    void visit(const ast::TaggedTemplate& n) override {
        Walker::visit(n);
        noteCallAfterEscape();
    }
    void noteCallAfterEscape() {
        if (thisEscaped) openAt = std::min(openAt, names.size());
    }

    void visit(const ast::FunctionExpr& n) override {
        if (n.isArrow) Walker::visit(n);
    }
    void visit(const ast::FunctionDecl&) override {}
    void visit(const ast::ClassDecl&) override {}
    void visit(const ast::ClassExpr&) override {}

    // `if (c) { this.k = a } else { this.k = b }` installs `k` on EVERY path,
    // at the same position on every path, so it is not conditional at all. It
    // is also how three.js's Texture writes `colorSpace`, and reading it as a
    // hole refused Texture and the nine classes that extend it.
    //
    // The test is deliberately literal: run each arm into its own copy of this
    // walker and compare the sequences of names the arms ADDED. Equal means one
    // order, whatever the condition did; anything else is a genuine hole.
    void visit(const ast::IfStmt& n) override {
        if (n.condition) n.condition->accept(*this);
        if (n.elseBody.empty()) {
            branched([&] { walkList(n.thenBody); });
            return;
        }
        ThisWriteWalker thenArm = arm();
        thenArm.walkList(n.thenBody);
        ThisWriteWalker elseArm = arm();
        elseArm.walkList(n.elseBody);
        absorb(thenArm);
        absorb(elseArm);

        const std::vector<std::string> addedThen(thenArm.names.begin() + names.size(),
                                                 thenArm.names.end());
        const std::vector<std::string> addedElse(elseArm.names.begin() + names.size(),
                                                 elseArm.names.end());
        if (addedThen == addedElse) {
            for (const auto& name : addedThen) add(name, true);
            return;
        }
        branched([&] {
            for (const auto& name : addedThen) add(name, true);
            for (const auto& name : addedElse) add(name, true);
        });
    }
    void visit(const ast::WhileStmt& n) override { branched([&] { Walker::visit(n); }); }
    void visit(const ast::DoWhileStmt& n) override { branched([&] { Walker::visit(n); }); }
    void visit(const ast::ForStmt& n) override { branched([&] { Walker::visit(n); }); }
    void visit(const ast::ForInStmt& n) override { branched([&] { Walker::visit(n); }); }
    void visit(const ast::ForOfStmt& n) override { branched([&] { Walker::visit(n); }); }
    void visit(const ast::SwitchStmt& n) override { branched([&] { Walker::visit(n); }); }
    void visit(const ast::TryStmt& n) override { branched([&] { Walker::visit(n); }); }
    void visit(const ast::Ternary& n) override {
        n.condition->accept(*this);
        branched([&] {
            n.thenExpr->accept(*this);
            n.elseExpr->accept(*this);
        });
    }

    void visit(const ast::Binary& n) override {
        if (ast::isAssignOp(n.op)) recordTarget(*n.lhs);
        // `a && (this.x = 1)` and `a ?? (this.x = 1)` evaluate their right
        // operand conditionally, which is the same hazard the statement forms
        // above carry.
        const bool shortCircuit = n.op == ast::BinaryOp::LogicalAnd ||
                                  n.op == ast::BinaryOp::LogicalOr ||
                                  n.op == ast::BinaryOp::NullishCoalescing;
        if (shortCircuit) {
            n.lhs->accept(*this);
            branched([&] { n.rhs->accept(*this); });
            return;
        }
        const bool compares = n.op == ast::BinaryOp::Eq || n.op == ast::BinaryOp::StrictEq ||
                              n.op == ast::BinaryOp::Ne || n.op == ast::BinaryOp::StrictNe;
        if (compares) {
            if (!isThis(n.lhs.get())) n.lhs->accept(*this);
            if (!isThis(n.rhs.get())) n.rhs->accept(*this);
            return;
        }
        Walker::visit(n);
    }
    void visit(const ast::Unary& n) override {
        const bool mutating = n.op == ast::UnaryOp::PreInc || n.op == ast::UnaryOp::PreDec ||
                              n.op == ast::UnaryOp::PostInc || n.op == ast::UnaryOp::PostDec;
        // `this.n++` on an absent property installs it, exactly as an
        // assignment does, so it is a transition and belongs in the order.
        if (mutating) recordTarget(*n.operand);
        Walker::visit(n);
    }
    void visit(const ast::DestructuringAssign& n) override {
        // `({a: this.x} = o)` is a write to `this.x` through a pattern. Rather
        // than model pattern targets, refuse: it does not appear in the code
        // this analysis is for, and a wrong order is worse than no order.
        if (patternTouchesThis(n.pattern.get())) sawUnmodellableWrite = true;
        Walker::visit(n);
    }
    void visit(const ast::Call& n) override {
        // The modelled reflections name `this` as an argument without letting
        // it escape: what they install is counted here.
        const bool escapedBefore = thisEscaped;
        const size_t openBefore = openAt;
        const size_t installedBefore = names.size();
        if (const auto* defined = modellableThisDefine(n)) {
            for (const auto& entry : *defined) add(entry.first, entry.second);
            Walker::visit(n);
            thisEscaped = escapedBefore;
            openAt = openBefore;
            noteCallAfterEscape();
            return;
        }
        if (const auto* accessors = modellableThisAccessor(n)) {
            for (const auto& name : *accessors) {
                if (std::find(accessorNames.begin(), accessorNames.end(), name) == accessorNames.end()) {
                    accessorNames.push_back(name);
                }
            }
            Walker::visit(n);
            thisEscaped = escapedBefore;
            openAt = openBefore;
            noteCallAfterEscape();
            return;
        }
        if (isThisReflectionCall(n)) sawThisReflection = true;
        // `this.set(...)` / `super.copy(...)`: inline helper methods when possible
        if (const auto* m = dynamic_cast<const ast::MemberAccess*>(n.callee.get())) {
            if (isThis(m->object.get()) && !m->isPrivate) {
                if (methods != nullptr && !inlining.count(m->property)) {
                    for (const auto& method : *methods) {
                        if (method.name == m->property && method.fn != nullptr && !method.isStatic) {
                            inlining.insert(m->property);
                            walkList(method.fn->body);
                            inlining.erase(m->property);
                            break;
                        }
                    }
                }
                noteThisCall(m->property);
            }
        } else if (const auto* sm = dynamic_cast<const ast::SuperMember*>(n.callee.get())) {
            noteThisCall(sm->property);
        }
        Walker::visit(n);
        // An escaped `this` may gain fields anywhere inside the call,
        // including in front of what an inlined method body installed.
        if (thisEscaped) openAt = std::min(openAt, installedBefore);
    }

    // Callee name -> how many fields were installed when the call was made.
    // The count is what separates a call in the MIDDLE of the order, which can
    // shift every field after it, from one at the END, which cannot shift
    // anything because there is nothing after it.
    std::map<std::string, size_t> thisCalls;

    void noteThisCall(const std::string& name) {
        thisCalls.emplace(name, names.size());
    }

private:
    void recordTarget(const ast::Expr& lhs) {
        if (const auto* m = dynamic_cast<const ast::MemberAccess*>(&lhs)) {
            if (!isThis(m->object.get())) return;
            // A private name is not a property key: no shape carries `#x`, so
            // it takes no slot and must not appear in the layout.
            if (m->isPrivate) return;
            add(m->property, true);
            return;
        }
        if (const auto* ix = dynamic_cast<const ast::IndexAccess*>(&lhs)) {
            if (!isThis(ix->object.get())) return;
            // A literal string index is an ordinary named write; anything else
            // is a key this analysis cannot name, and the order after it is
            // unknown.
            if (const auto* s = dynamic_cast<const ast::StringLit*>(ix->index.get())) {
                add(s->value, true);
                return;
            }
            sawUnmodellableWrite = true;
        }
    }

    // A walker over one arm of an if/else: it starts from the names already
    // installed — so an arm re-assigning an existing field adds nothing — and
    // at branch depth zero, so a nested conditional inside the arm is judged on
    // its own terms and reported back through `absorb`.
    ThisWriteWalker arm() const {
        ThisWriteWalker w;
        w.methods = methods;
        w.inlining = inlining;
        w.names = names;
        w.writable = writable;
        w.lateFields = lateFields;
        w.accessorNames = accessorNames;
        return w;
    }

    void absorb(const ThisWriteWalker& other) {
        sawUnmodellableWrite = sawUnmodellableWrite || other.sawUnmodellableWrite;
        sawThisReflection = sawThisReflection || other.sawThisReflection;
        sawConditionalNewField = sawConditionalNewField || other.sawConditionalNewField;
        thisEscaped = thisEscaped || other.thisEscaped;
        openAt = std::min(openAt, other.openAt);
        for (const auto& lf : other.lateFields) {
            if (std::find(lateFields.begin(), lateFields.end(), lf) == lateFields.end()) {
                lateFields.push_back(lf);
            }
        }
        for (const auto& acc : other.accessorNames) {
            if (std::find(accessorNames.begin(), accessorNames.end(), acc) == accessorNames.end()) {
                accessorNames.push_back(acc);
            }
        }
        thisCalls.insert(other.thisCalls.begin(), other.thisCalls.end());
    }

    template <typename F>
    void branched(F&& body) {
        ++branchDepth_;
        body();
        --branchDepth_;
    }

    void add(const std::string& name, bool isWritable) {
        if (std::find(names.begin(), names.end(), name) != names.end()) return;
        if (branchDepth_ != 0) {
            if (std::find(lateFields.begin(), lateFields.end(), name) == lateFields.end()) {
                lateFields.push_back(name);
            }
            return;
        }
        names.push_back(name);
        writable.push_back(isWritable);
    }

    uint32_t branchDepth_ = 0;

    static bool isThis(const ast::Expr* e) {
        return dynamic_cast<const ast::ThisExpr*>(e) != nullptr;
    }

    static bool patternTouchesThis(const ast::BindingPattern* p) {
        if (!p) return false;
        for (const auto& elem : p->elements) {
            if (elem.target && isThis(memberBase(elem.target.get()))) return true;
            if (patternTouchesThis(elem.pattern.get())) return true;
        }
        return false;
    }

    static const ast::Expr* memberBase(const ast::Expr* e) {
        if (const auto* m = dynamic_cast<const ast::MemberAccess*>(e)) return m->object.get();
        if (const auto* ix = dynamic_cast<const ast::IndexAccess*>(e)) return ix->object.get();
        return nullptr;
    }

    // A plain data descriptor: `{ value: v }`, `{ value: v, writable: true }`.
    // Refuses anything with a computed key, a spread, a method, or a `get`/`set`
    // — the last because that is an accessor, which is not this shape at all.
    //
    // `outWritable` receives what the descriptor says about writability, which
    // is 6.2.6.5's default — FALSE — unless it says `writable: true` in so many
    // words. Anything less literal than a boolean literal answers false too:
    // being wrong here costs a guard that never matches, and false is the answer
    // that is right for `{value: n}`, the form the library actually uses.
    static bool isDataDescriptor(const ast::Expr* e, bool& outWritable) {
        outWritable = false;
        const auto* lit = dynamic_cast<const ast::ObjectLit*>(e);
        if (lit == nullptr) return false;
        for (const auto& p : lit->props) {
            if (p.computed() || p.accessor != ast::AccessorKind::None || p.coverInitialized) {
                return false;
            }
            if (p.key == "get" || p.key == "set" || p.key.empty()) return false;
            if (p.key == "writable") {
                const auto* b = dynamic_cast<const ast::BoolLit*>(p.value.get());
                outWritable = b != nullptr && b->value;
            }
        }
        return true;
    }

    static bool isAccessorDescriptor(const ast::Expr* e) {
        const auto* lit = dynamic_cast<const ast::ObjectLit*>(e);
        if (lit == nullptr) return false;
        for (const auto& p : lit->props) {
            if (p.computed() || p.coverInitialized) return false;
            if (p.key == "get" || p.key == "set" || p.accessor != ast::AccessorKind::None) {
                return true;
            }
        }
        return false;
    }

    // The keys a modellable `defineProperty`/`defineProperties` on `this`
    // installs, in order, each with its writability, or null when the call is
    // not one.
    const std::vector<std::pair<std::string, bool>>* modellableThisDefine(
        const ast::Call& call) {
        const auto* m = dynamic_cast<const ast::MemberAccess*>(call.callee.get());
        if (m == nullptr) return nullptr;
        const auto* base = dynamic_cast<const ast::Ident*>(m->object.get());
        if (base == nullptr || (base->name != "Object" && base->name != "Reflect")) return nullptr;
        if (call.args.size() < 2 || !isThis(call.args[0].get())) return nullptr;

        definedScratch_.clear();
        bool isWritable = false;
        if (m->property == "defineProperty") {
            const auto* key = dynamic_cast<const ast::StringLit*>(call.args[1].get());
            if (key == nullptr || call.args.size() < 3) return nullptr;
            if (!isDataDescriptor(call.args[2].get(), isWritable)) return nullptr;
            definedScratch_.emplace_back(key->value, isWritable);
            return &definedScratch_;
        }
        if (m->property != "defineProperties") return nullptr;
        const auto* map = dynamic_cast<const ast::ObjectLit*>(call.args[1].get());
        if (map == nullptr) return nullptr;
        for (const auto& p : map->props) {
            if (p.computed() || p.key.empty() || p.accessor != ast::AccessorKind::None) {
                return nullptr;
            }
            if (!isDataDescriptor(p.value.get(), isWritable)) return nullptr;
            definedScratch_.emplace_back(p.key, isWritable);
        }
        return &definedScratch_;
    }

    const std::vector<std::string>* modellableThisAccessor(const ast::Call& call) {
        const auto* m = dynamic_cast<const ast::MemberAccess*>(call.callee.get());
        if (m == nullptr) return nullptr;
        const auto* base = dynamic_cast<const ast::Ident*>(m->object.get());
        if (base == nullptr || (base->name != "Object" && base->name != "Reflect")) return nullptr;
        if (call.args.size() < 2 || !isThis(call.args[0].get())) return nullptr;

        accessorScratch_.clear();
        if (m->property == "defineProperty") {
            const auto* key = dynamic_cast<const ast::StringLit*>(call.args[1].get());
            if (key == nullptr || call.args.size() < 3) return nullptr;
            if (!isAccessorDescriptor(call.args[2].get())) return nullptr;
            accessorScratch_.push_back(key->value);
            return &accessorScratch_;
        }
        if (m->property != "defineProperties") return nullptr;
        const auto* map = dynamic_cast<const ast::ObjectLit*>(call.args[1].get());
        if (map == nullptr) return nullptr;
        for (const auto& p : map->props) {
            if (p.computed() || p.key.empty()) return nullptr;
            if (!isAccessorDescriptor(p.value.get())) return nullptr;
            accessorScratch_.push_back(p.key);
        }
        return &accessorScratch_;
    }

    std::vector<std::pair<std::string, bool>> definedScratch_;
    std::vector<std::string> accessorScratch_;

    // `Object.defineProperty(this, ...)`, `Object.defineProperties(this, ...)`,
    // `Object.assign(this, ...)`, `Object.freeze(this)`, `Object.seal(this)`,
    // `Object.setPrototypeOf(this, ...)`, `Reflect.defineProperty(this, ...)`.
    // Any of them makes the slot numbering after it a runtime fact.
    static bool isThisReflectionCall(const ast::Call& call) {
        const auto* m = dynamic_cast<const ast::MemberAccess*>(call.callee.get());
        if (m == nullptr) return false;
        const auto* base = dynamic_cast<const ast::Ident*>(m->object.get());
        if (base == nullptr) return false;
        if (base->name != "Object" && base->name != "Reflect") return false;
        static const char* kNames[] = {"defineProperty", "defineProperties", "assign",
                                       "freeze",         "seal",             "preventExtensions",
                                       "setPrototypeOf"};
        bool named = false;
        for (const char* n : kNames) {
            if (m->property == n) named = true;
        }
        if (!named) return false;
        return !call.args.empty() && isThis(call.args[0].get());
    }
};

}  // namespace bronze::types::detail
