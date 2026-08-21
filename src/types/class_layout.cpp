// Class layouts, proven from the construction sequence.
//
// Three.js is disciplined ES6: a class whose constructor assigns every field it
// will ever have, in one order, on every path. That is the shape of program
// this file is written for, and the refusals below are the exact list of things
// that make the order un-modellable rather than a general-purpose bail-out.
//
// What the runtime does, and what this therefore has to reproduce: `this.x = v`
// on an object that has no `x` walks the shape transition tree, takes (or
// mints) the `x` edge, and the new node's `slot_index` is the previous node's
// `nextSlotIndex()` — i.e. slot N for the Nth distinct data property, counting
// from the ROOT shape of the constructor's `.prototype`. So the layout is the
// deduplicated sequence of property names installed on the instance, and
// "installed" for a class means, in ECMA-262 15.7.14 order:
//
//     base class's whole layout   (super() runs first and returns before any
//                                  derived field initializer or ctor statement)
//   ++ this class's field declarations, in declaration order
//   ++ this class's constructor body's `this.<name> = ...`, in source order
//
// Deduplicated keeping the FIRST occurrence, because a second write to a name
// the object already has is a slot store and not a transition.

#include "types/class_layout.h"

#include <algorithm>

#include "types/walk.h"

namespace bronze::types {

// One collected `class` node: everything `resolve` needs from the AST, so the
// resolution pass never walks it again.
struct ClassLayoutTable::FoundRef {
    std::string name;
    std::string superName;
    bool hasSuperExpr = false;
    const std::vector<ast::ClassMethod>* methods = nullptr;
};

namespace {

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
    std::vector<std::string> names;
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
    // A name introduced for the first time under a branch. Two instances of the
    // class then have two different property ORDERS — `this.x; if (c) this.y;
    // this.z` puts `z` at slot 2 or slot 1 depending on `c` — so no one layout
    // describes them.
    //
    // Refusing is a choice, and the alternative was available: the layout is
    // still a guess behind a shape compare, so an optimistic one would be
    // *correct*, just wrong half the time. It is refused because a wrong guess
    // is not free — the site pays a load and a compare that can never hit, on
    // top of the cache path it would have taken anyway — and a constructor with
    // a conditional field is exactly the case where "wrong half the time" is
    // the expected outcome rather than the unlucky one.
    bool sawConditionalNewField = false;

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
            for (const auto& name : addedThen) add(name);
            return;
        }
        branched([&] {
            for (const auto& name : addedThen) add(name);
            for (const auto& name : addedElse) add(name);
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
        if (const auto* defined = modellableThisDefine(n)) {
            // `Object.defineProperty(this, 'id', { value: n })` is a TRANSITION,
            // not a bail. `builtin_object_descriptor.cpp` routes a new key on a
            // shaped object through `setProp(defineOwn=true)`, so the property
            // lands in the shape tree at the next slot exactly as an assignment
            // would — with different attributes, which slot numbering does not
            // care about.
            //
            // Not an incidental case: three.js gives Object3D, BufferGeometry,
            // Material and Texture their `id` this way, and those four are the
            // roots of essentially every `extends` chain in the library. Reading
            // the call as a bail refused 56 classes directly and more through
            // the chain; reading it as what it is costs nine lines.
            //
            // An ACCESSOR descriptor is deliberately not modelled: it takes a
            // different runtime path, and a slot claimed over it could not be
            // published anyway.
            for (const auto& name : *defined) add(name);
            Walker::visit(n);
            return;
        }
        if (isThisReflectionCall(n)) sawThisReflection = true;
        // `this.set(...)` / `super.copy(...)`: the callee runs with `this`
        // half-built, so anything IT installs lands in the middle of the
        // constructor's order rather than after it. Recorded by name; the
        // caller decides whether that name is a method that installs anything.
        if (const auto* m = dynamic_cast<const ast::MemberAccess*>(n.callee.get())) {
            if (isThis(m->object.get()) && !m->isPrivate) noteThisCall(m->property);
        } else if (const auto* sm = dynamic_cast<const ast::SuperMember*>(n.callee.get())) {
            noteThisCall(sm->property);
        }
        Walker::visit(n);
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
            add(m->property);
            return;
        }
        if (const auto* ix = dynamic_cast<const ast::IndexAccess*>(&lhs)) {
            if (!isThis(ix->object.get())) return;
            // A literal string index is an ordinary named write; anything else
            // is a key this analysis cannot name, and the order after it is
            // unknown.
            if (const auto* s = dynamic_cast<const ast::StringLit*>(ix->index.get())) {
                add(s->value);
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
        w.names = names;
        return w;
    }

    void absorb(const ThisWriteWalker& other) {
        sawUnmodellableWrite = sawUnmodellableWrite || other.sawUnmodellableWrite;
        sawThisReflection = sawThisReflection || other.sawThisReflection;
        sawConditionalNewField = sawConditionalNewField || other.sawConditionalNewField;
        thisCalls.insert(other.thisCalls.begin(), other.thisCalls.end());
    }

    template <typename F>
    void branched(F&& body) {
        ++branchDepth_;
        body();
        --branchDepth_;
    }

    void add(const std::string& name) {
        if (std::find(names.begin(), names.end(), name) != names.end()) return;
        if (branchDepth_ != 0) sawConditionalNewField = true;
        names.push_back(name);
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
    static bool isDataDescriptor(const ast::Expr* e) {
        const auto* lit = dynamic_cast<const ast::ObjectLit*>(e);
        if (lit == nullptr) return false;
        for (const auto& p : lit->props) {
            if (p.computed() || p.accessor != ast::AccessorKind::None || p.coverInitialized) {
                return false;
            }
            if (p.key == "get" || p.key == "set" || p.key.empty()) return false;
        }
        return true;
    }

    // The keys a modellable `defineProperty`/`defineProperties` on `this`
    // installs, in order, or null when the call is not one.
    const std::vector<std::string>* modellableThisDefine(const ast::Call& call) {
        const auto* m = dynamic_cast<const ast::MemberAccess*>(call.callee.get());
        if (m == nullptr) return nullptr;
        const auto* base = dynamic_cast<const ast::Ident*>(m->object.get());
        if (base == nullptr || (base->name != "Object" && base->name != "Reflect")) return nullptr;
        if (call.args.size() < 2 || !isThis(call.args[0].get())) return nullptr;

        definedScratch_.clear();
        if (m->property == "defineProperty") {
            const auto* key = dynamic_cast<const ast::StringLit*>(call.args[1].get());
            if (key == nullptr || call.args.size() < 3) return nullptr;
            if (!isDataDescriptor(call.args[2].get())) return nullptr;
            definedScratch_.push_back(key->value);
            return &definedScratch_;
        }
        if (m->property != "defineProperties") return nullptr;
        const auto* map = dynamic_cast<const ast::ObjectLit*>(call.args[1].get());
        if (map == nullptr) return nullptr;
        for (const auto& p : map->props) {
            if (p.computed() || p.key.empty() || p.accessor != ast::AccessorKind::None) {
                return nullptr;
            }
            if (!isDataDescriptor(p.value.get())) return nullptr;
            definedScratch_.push_back(p.key);
        }
        return &definedScratch_;
    }

    std::vector<std::string> definedScratch_;

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

// What one `this.<field> = <rhs>` says about the field's type.
//
// Syntactic, one level deep, and deliberately so: the flow analysis that could
// answer this properly runs AFTER this table is built (it consumes it), so
// asking it here would be a cycle. The forms recognised are the ones that carry
// a class identity or a number — which is the whole of what a later fixed-slot
// or unboxed-slot decision can use — and everything else answers `Dynamic`,
// which poisons the join and is always the safe answer.
Type harvestFieldType(const ast::Expr& rhs, const std::map<std::string, size_t>& classByName,
                      const std::vector<ClassLayout>& classes) {
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
                    const std::vector<ClassLayout>& classes)
        : out_(out), classByName_(classByName), classes_(classes) {}

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
                    record(m->property, harvestFieldType(*n.rhs, classByName_, classes_));
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
};

// Every `class` in the program, at any nesting depth. Nesting is not a problem
// for identity — a class expression evaluated twice makes two prototypes, but
// both instances agree on the layout, which is what is claimed here.
class ClassCollector final : public Walker {
public:
    using Found = ClassLayoutTable::FoundRef;
    std::vector<Found> found;

    void visit(const ast::ClassDecl& n) override {
        found.push_back(make(n.name, n.superName, n.superClass.get(), n.methods));
        Walker::visit(n);
    }
    void visit(const ast::ClassExpr& n) override {
        // An anonymous class expression has no name to resolve `extends`
        // against and no `new <name>` site to type, so it contributes nothing.
        if (!n.name.empty()) {
            found.push_back(make(n.name, n.superName, n.superClass.get(), n.methods));
        }
        Walker::visit(n);
    }

private:
    // Which spelling of the base names the binding this analysis can look up.
    //
    // `ClassDecl::superName` is what the PARSER resolved, and it is the wrong
    // one after module linking: the linker flattens the graph by renaming every
    // module-level binding into `mod<N>.<local>`, and it rewrites `superClass`
    // — an expression — while leaving `superName` at the local spelling the
    // parser saw. Reading `superName` therefore failed to resolve every
    // cross-module `extends` in the program, which in three.js is essentially
    // all of them: 157 of 169 refusals were this one line.
    static Found make(const std::string& name, const std::string& superName,
                      const ast::Expr* superClass,
                      const std::vector<ast::ClassMethod>& methods) {
        Found f;
        f.name = name;
        f.methods = &methods;
        if (superClass != nullptr) {
            if (const auto* id = dynamic_cast<const ast::Ident*>(superClass)) {
                f.superName = id->name;
            } else {
                // `class X extends (expr)`: a base this analysis cannot name.
                f.hasSuperExpr = true;
            }
            return f;
        }
        f.superName = superName;
        return f;
    }
};

const ast::ClassMethod* constructorOf(const std::vector<ast::ClassMethod>& methods) {
    for (const auto& m : methods) {
        if (m.isConstructor && m.fn) return &m;
    }
    return nullptr;
}

}  // namespace

void ClassLayoutTable::build(const ast::Module& module, ShapeClassTable& shapes) {
    shapes_ = &shapes;

    ClassCollector collector;
    for (const auto& stmt : module.body) {
        if (stmt) stmt->accept(collector);
    }

    // Pass 1: names and identities. Identity has to exist for every class
    // before any layout is resolved, because a field's harvested type can name
    // another class and `extends` resolves by name.
    for (const auto& f : collector.found) {
        if (byName_.count(f.name) != 0) {
            // Two classes bound to one name in one program. The linker's
            // renaming makes this impossible for module-level declarations; a
            // nested one can still do it, and the first wins, exactly as
            // `indexByName` lets the first top-level function win.
            continue;
        }
        ClassLayout cl;
        cl.name = f.name;
        cl.superName = f.superName;
        classes_.push_back(std::move(cl));
        byName_.emplace(f.name, classes_.size() - 1);
    }

    // Pass 2: layouts. `resolve` recurses through `extends`, so the loop order
    // does not matter and a base declared after its derived class still works.
    resolved_.assign(classes_.size(), false);
    // The collector's order matches `classes_`' order only for first
    // occurrences, which is what `byName_` already indexes.
    std::vector<const FoundRef*> byIndex(classes_.size(), nullptr);
    for (const auto& f : collector.found) {
        const auto it = byName_.find(f.name);
        if (it != byName_.end() && byIndex[it->second] == nullptr) byIndex[it->second] = &f;
    }
    found_ = std::move(byIndex);
    for (size_t i = 0; i < classes_.size(); ++i) {
        std::set<size_t> resolving;
        resolve(i, resolving);
    }

    // Pass 3: field types. Deferred to its own pass because a field's type can
    // name a class whose own identity is only minted in pass 2.
    for (size_t i = 0; i < classes_.size(); ++i) {
        const FoundRef* f = found_[i];
        if (f == nullptr || f->methods == nullptr) continue;
        FieldTypeWalker walker(classes_[i].fieldTypes, byName_, classes_);
        for (const auto& m : *f->methods) {
            if (m.isStatic) continue;
            if (m.fn) {
                walker.walkList(m.fn->body);
            } else if (m.init && !m.name.empty()) {
                classes_[i].fieldTypes[m.name] =
                    harvestFieldType(*m.init, byName_, classes_);
            }
        }
    }
}

void ClassLayoutTable::resolve(size_t index, std::set<size_t>& resolving) {
    if (resolved_[index]) return;
    ClassLayout& cl = classes_[index];
    const FoundRef* f = found_[index];

    if (!resolving.insert(index).second) {
        // A cycle through `extends`. The program throws at run time; here it
        // is simply a layout nobody can name.
        cl.refusal = "cyclic extends";
        resolved_[index] = true;
        return;
    }

    std::vector<std::string> fields;
    bool proven = true;
    std::string refusal;
    std::map<std::string, size_t> ctorThisCalls;
    size_t ctorFieldCount = 0;
    bool baseEndsInLateCall = false;
    size_t baseFieldCount = 0;

    if (!cl.superName.empty()) {
        const auto it = byName_.find(cl.superName);
        if (it == byName_.end()) {
            // `extends` a name this program does not declare as a class — a
            // builtin (`Array`, `Error`), a host global, an imported binding
            // that is not a class declaration. The base's own field list is
            // unknown, so every derived slot number is unknown too.
            proven = false;
            refusal = "base class is not a modelled class";
        } else {
            resolve(it->second, resolving);
            classes_[it->second].extended = true;
            const ClassLayout& base = classes_[it->second];
            if (!base.layoutProven) {
                // The base's OWN reason is not repeated: a chain six deep
                // produced a histogram row six parentheses long that named the
                // same root cause as its base's row. Naming the base is what a
                // reader needs — the root cause is one lookup away.
                proven = false;
                refusal = "base class layout refused: " + base.name;
            } else {
                fields = base.fields;
                baseFieldCount = fields.size();
                // A base method's late field is a late field of every derived
                // class too — instances of the derived class are what the
                // method runs on — and so is the method name, for the
                // constructor-calls check below.
                cl.lateFields = base.lateFields;
                cl.lateMethods = base.lateMethods;
                cl.lateCallTail = base.lateCallTail;
                baseEndsInLateCall = base.lateCallTail;
            }
        }
    } else if (f != nullptr && f->hasSuperExpr) {
        // `class X extends (expr)` with no name the parser could resolve.
        proven = false;
        refusal = "base class is a computed expression";
    }

    if (proven && f != nullptr && f->methods != nullptr) {
        auto add = [&fields](const std::string& name) {
            if (std::find(fields.begin(), fields.end(), name) == fields.end()) {
                fields.push_back(name);
            }
        };

        // 15.7.14: field initializers run before the constructor body (for a
        // base class) or immediately after `super()` returns (for a derived
        // one). Either way they precede every statement of this constructor.
        for (const auto& m : *f->methods) {
            if (!m.isField || m.isStatic) continue;
            if (m.computed()) {
                proven = false;
                refusal = "class field has a computed key";
                break;
            }
            if (m.isPrivate()) continue;  // no shape carries a private name
            add(m.name);
        }

        if (proven) {
            if (const ast::ClassMethod* ctor = constructorOf(*f->methods)) {
                ThisWriteWalker walker;
                walker.walkList(ctor->fn->body);
                if (walker.sawThisReflection) {
                    proven = false;
                    refusal = "constructor reflects on this (defineProperty/assign/freeze)";
                } else if (walker.sawUnmodellableWrite) {
                    proven = false;
                    refusal = "constructor writes a computed key on this";
                } else if (walker.sawConditionalNewField) {
                    proven = false;
                    refusal = "constructor assigns a field conditionally";
                } else {
                    for (const auto& name : walker.names) add(name);
                    ctorThisCalls = std::move(walker.thisCalls);
                    ctorFieldCount = walker.names.size();
                }
            }
        }
    }

    // Methods get to REFUSE, and to name fields the layout must NOT contain.
    //
    // A field a method installs — three.js's `if (this._listeners === undefined)
    // this._listeners = {}` is the canonical one, and it is the single most
    // common shape in the library — lands AFTER every field the constructor
    // installed, because the constructor ran first. So it cannot move a
    // constructor field's slot, and the constructor's layout is still exactly
    // right for the fields it does name. What it does mean is that instances of
    // the class come in two shapes, and a site pins one of them: the population
    // that took the append misses the guard and falls into the cache, which is
    // what a cache is for.
    //
    // Excluding rather than refusing is worth stating plainly, because the
    // first cut refused and it cost more than everything else this chunk did
    // put together: `EventDispatcher._listeners` alone refused 105 of three.js's
    // classes through the `extends` chain, Quaternion and Euler among them.
    // Slot numbering, which is the only thing the layout claims, was never in
    // doubt for any of them.
    //
    // A method that REFLECTS on `this`, or writes a computed key, is different
    // and still refuses: those can reorder or dictionary-ize what is already
    // there, and that does move a constructor field's slot.
    if (proven && f != nullptr && f->methods != nullptr) {
        for (const auto& m : *f->methods) {
            if (m.isStatic || m.isConstructor || !m.fn) continue;
            ThisWriteWalker walker;
            walker.walkList(m.fn->body);
            if (walker.sawThisReflection) {
                proven = false;
                refusal = "a method reflects on this (defineProperty/assign/freeze)";
                break;
            }
            // A computed write in a method — `Material.setValues`' `this[key] =
            // v`, which is 14 of three.js's classes through one `extends` chain
            // — is not a refusal for the same reason a late field is not: an
            // assignment either hits a slot that exists or appends one, and
            // neither moves a field the constructor installed. What it costs is
            // that the appended NAMES are unknown, which is only a reason not
            // to claim slots for names the layout never had.
            (void)walker.sawUnmodellableWrite;
            bool installsLate = false;
            for (const auto& name : walker.names) {
                if (std::find(fields.begin(), fields.end(), name) != fields.end()) continue;
                installsLate = true;
                if (std::find(cl.lateFields.begin(), cl.lateFields.end(), name) ==
                    cl.lateFields.end()) {
                    cl.lateFields.push_back(name);
                }
            }
            if (installsLate && std::find(cl.lateMethods.begin(), cl.lateMethods.end(), m.name) ==
                                    cl.lateMethods.end()) {
                cl.lateMethods.push_back(m.name);
            }
        }
    }

    // The one way a late field can still move a constructor field's slot: the
    // constructor CALLS the method that installs it, so the append happens
    // while the order is still being built rather than after it is finished.
    // `this.setIndex(...)` in a geometry constructor is the shape to picture —
    // harmless there, because `index` is a constructor field, and fatal if the
    // callee had installed something new.
    if (proven && !ctorThisCalls.empty()) {
        for (const auto& [called, installedByThen] : ctorThisCalls) {
            const bool late =
                std::find(cl.lateMethods.begin(), cl.lateMethods.end(), called) !=
                cl.lateMethods.end();
            if (!late) continue;
            if (installedByThen == ctorFieldCount) {
                // A TAIL call: `Mesh`'s constructor ends with
                // `this.updateMorphTargets()`, and so do `Line`'s and
                // `Points`'. Nothing this class installs comes after it, so
                // this class's own layout is exactly right — but a class that
                // extends it writes its fields after the append, and that one
                // has to refuse. The flag carries that obligation down.
                cl.lateCallTail = true;
                continue;
            }
            proven = false;
            refusal = "constructor calls a method that installs a field ('" + called + "')";
            break;
        }
    }

    // The base's constructor ended in a call that may append. Anything THIS
    // class installs therefore lands after an unknown number of appends, and
    // its slot is unknown. A class that installs nothing of its own is fine and
    // carries the obligation further down.
    if (proven && baseEndsInLateCall && fields.size() > baseFieldCount) {
        proven = false;
        refusal = "base constructor ends in a method that installs a field";
    }

    // The identity is minted whatever the layout verdict is: a refused class is
    // still one compile-time object kind, and the inline-cache form it already
    // licensed is unaffected by anything decided here.
    cl.shapeClass = shapes_->intern(cl.name, std::vector<std::string>(fields));
    byShape_.emplace(cl.shapeClass, index);

    cl.fields = std::move(fields);
    cl.layoutProven = proven;
    cl.refusal = proven ? std::string() : refusal;
    resolved_[index] = true;
    resolving.erase(index);
}

const ClassLayout* ClassLayoutTable::byName(const std::string& name) const {
    const auto it = byName_.find(name);
    return it == byName_.end() ? nullptr : &classes_[it->second];
}

const ClassLayout* ClassLayoutTable::byShapeClass(ShapeClassId id) const {
    if (id == kNoShapeClass) return nullptr;
    const auto it = byShape_.find(id);
    return it == byShape_.end() ? nullptr : &classes_[it->second];
}

uint32_t ClassLayoutTable::slotOf(ShapeClassId id, const std::string& field) const {
    const ClassLayout* cl = byShapeClass(id);
    if (cl == nullptr || !cl->layoutProven) return kNoSlot;
    const auto it = std::find(cl->fields.begin(), cl->fields.end(), field);
    if (it == cl->fields.end()) return kNoSlot;
    return static_cast<uint32_t>(it - cl->fields.begin());
}

Type ClassLayoutTable::fieldTypeOf(ShapeClassId id, const std::string& field) const {
    const ClassLayout* cl = byShapeClass(id);
    if (cl == nullptr) return Type::dynamic();
    const auto it = cl->fieldTypes.find(field);
    return it == cl->fieldTypes.end() ? Type::dynamic() : it->second;
}

bool ClassLayoutTable::isExtended(ShapeClassId id) const {
    const ClassLayout* cl = byShapeClass(id);
    return cl != nullptr && cl->extended;
}

std::map<std::string, uint32_t> ClassLayoutTable::refusalHistogram() const {
    std::map<std::string, uint32_t> out;
    for (const auto& cl : classes_) {
        if (cl.layoutProven) continue;
        ++out[cl.refusal.empty() ? std::string("unknown") : cl.refusal];
    }
    return out;
}

}  // namespace bronze::types
