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
#include <utility>

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
    // Parallel to `names`: whether that install makes a WRITABLE data property.
    // False only for a `defineProperty` descriptor that does not say
    // `writable: true`, which is the default the spec gives an unstated
    // attribute and the one three.js's `id` relies on.
    std::vector<bool> writable;
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
            for (const auto& entry : *defined) add(entry.first, entry.second);
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
        w.names = names;
        w.writable = writable;
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

    void add(const std::string& name, bool isWritable) {
        if (std::find(names.begin(), names.end(), name) != names.end()) return;
        if (branchDepth_ != 0) sawConditionalNewField = true;
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

    std::vector<std::pair<std::string, bool>> definedScratch_;

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

    buildFamilies();

    // The class members each class declares, kept past `build` so the field-type
    // harvest can be re-run as constructor parameter types widen.
    methodsByIndex_.assign(classes_.size(), nullptr);
    for (size_t i = 0; i < classes_.size(); ++i) {
        if (found_[i] != nullptr) methodsByIndex_[i] = found_[i]->methods;
    }
    found_.clear();

    // Pass 3: field types. Deferred to its own pass because a field's type can
    // name a class whose own identity is only minted in pass 2.
    harvestFieldTypes(nullptr);
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
    std::vector<bool> fieldWritable;
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
                fieldWritable = base.fieldWritable;
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

    // Accessor names are collected whether or not the layout is proven, and
    // before every refusal below: they are what a PRIMITIVE field claim is
    // checked against (`fieldValueCandidate`), and that question is asked of a
    // class through its whole `extends` chain, so a link that refused its own
    // layout still has to answer it.
    if (f != nullptr && f->methods != nullptr) {
        for (const auto& m : *f->methods) {
            if (m.accessor == ast::AccessorKind::None || m.name.empty()) continue;
            if (std::find(cl.accessorNames.begin(), cl.accessorNames.end(), m.name) ==
                cl.accessorNames.end()) {
                cl.accessorNames.push_back(m.name);
            }
        }
    }

    if (proven && f != nullptr && f->methods != nullptr) {
        auto add = [&fields, &fieldWritable](const std::string& name, bool isWritable) {
            if (std::find(fields.begin(), fields.end(), name) == fields.end()) {
                fields.push_back(name);
                fieldWritable.push_back(isWritable);
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
            add(m.name, true);
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
                    for (size_t i = 0; i < walker.names.size(); ++i) {
                        add(walker.names[i], walker.writable[i]);
                    }
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

    // THE FAMILY INVARIANT, checked rather than assumed: this class's layout
    // has to BEGIN with its base's, name for name, or a site in a base method
    // that accepts a subclass shape would read the wrong slot.
    //
    // It holds by construction above — `super()` runs to completion before any
    // derived field initializer (15.7.14), the base's fields seed `fields`, and
    // `add` deduplicates so a derived class re-assigning an inherited name does
    // not move it. That is exactly why it is worth a check: the whole family
    // mechanism rests on a property of this loop, and a future edit to the loop
    // that broke it would otherwise be silent. A violation refuses the class
    // rather than firing an assertion — an unprovable layout is a thing this
    // file already knows how to say.
    if (proven && !cl.superName.empty()) {
        const auto it = byName_.find(cl.superName);
        if (it != byName_.end() && classes_[it->second].layoutProven) {
            const ClassLayout& base = classes_[it->second];
            const bool prefixOk =
                fields.size() >= base.fields.size() &&
                std::equal(base.fields.begin(), base.fields.end(), fields.begin()) &&
                std::equal(base.fieldWritable.begin(), base.fieldWritable.end(),
                           fieldWritable.begin());
            if (!prefixOk) {
                proven = false;
                refusal = "layout does not extend the base's prefix";
            }
        }
    }

    // The identity is minted whatever the layout verdict is: a refused class is
    // still one compile-time object kind, and the inline-cache form it already
    // licensed is unaffected by anything decided here.
    cl.shapeClass = shapes_->intern(cl.name, std::vector<std::string>(fields));
    byShape_.emplace(cl.shapeClass, index);

    cl.fields = std::move(fields);
    cl.fieldWritable = std::move(fieldWritable);
    cl.layoutProven = proven;
    cl.refusal = proven ? std::string() : refusal;
    resolved_[index] = true;
    resolving.erase(index);
}

// The layout-family forest: proven classes, linked by `extends`, numbered in
// preorder so that a class's descendants occupy [familyIndex, familyIndex +
// familySpan] and a guard is one unsigned range compare.
//
// Two classes are left out, both because their field list would recognise
// shapes it has no business recognising:
//
//   - a class whose layout was refused. There is nothing to verify a shape
//     against, so it can neither be stamped nor guard.
//   - a class with NO fields. Its field list is the empty prefix, which every
//     shape in the program begins with, so it would stamp every object with its
//     own id. It has no slot to claim either, so nothing is lost — and its
//     proven subclasses become forest roots in their own right rather than
//     being dragged out with it.
void ClassLayoutTable::buildFamilies() {
    std::map<size_t, std::vector<size_t>> children;
    std::vector<size_t> roots;
    auto inForest = [this](size_t i) {
        return classes_[i].layoutProven && !classes_[i].fields.empty();
    };
    for (size_t i = 0; i < classes_.size(); ++i) {
        if (!inForest(i)) continue;
        const auto parent = byName_.find(classes_[i].superName);
        if (!classes_[i].superName.empty() && parent != byName_.end() && inForest(parent->second)) {
            children[parent->second].push_back(i);
        } else {
            roots.push_back(i);
        }
    }
    uint32_t next = 0;
    for (size_t root : roots) next = numberSubtree(root, next, children);

    preorder_.assign(next, nullptr);
    for (const ClassLayout& cl : classes_) {
        if (cl.familyIndex != ClassLayout::kNoFamily) preorder_[cl.familyIndex] = &cl;
    }
}

uint32_t ClassLayoutTable::numberSubtree(size_t index, uint32_t next,
                                         const std::map<size_t, std::vector<size_t>>& children) {
    const uint32_t self = next++;
    classes_[index].familyIndex = self;
    const auto kids = children.find(index);
    if (kids != children.end()) {
        for (size_t child : kids->second) next = numberSubtree(child, next, children);
    }
    // The span is what the subtree consumed: every id in (self, next) belongs to
    // a class that extends this one, transitively, and therefore lays this
    // one's fields out at this one's slots.
    classes_[index].familySpan = next - self - 1;
    return next;
}

const ClassLayout* ClassLayoutTable::familyMemberOf(ShapeClassId id) const {
    const ClassLayout* cl = byShapeClass(id);
    if (cl == nullptr || cl->familyIndex == ClassLayout::kNoFamily) return nullptr;
    return cl;
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

// Everything about the CLASS that a primitive field claim needs, which is
// everything except the program-wide write audit (field_audit.h).
//
// Three conditions, and each is a way the claim has failed in practice:
//
//   - the layout is proven, and `field` is in it. `fieldTypes` also carries
//     the fields a METHOD installs, which the constructor does not: reading one
//     off a fresh instance produces `undefined`, and a `number` claim over that
//     is the same miscompile in a different disguise.
//   - no ACCESSOR of that name anywhere on the prototype chain. A getter
//     answers the read with a call, so the harvest describes a slot the read
//     never touches; a setter intercepts the constructor's own write, so the
//     own property the layout claims is never created at all.
//   - the chain is walked to the root, because an accessor two classes up
//     shadows exactly as well as one declared here.
bool ClassLayoutTable::fieldValueCandidate(ShapeClassId id, const std::string& field) const {
    const ClassLayout* cl = byShapeClass(id);
    if (cl == nullptr || !cl->layoutProven) return false;
    if (std::find(cl->fields.begin(), cl->fields.end(), field) == cl->fields.end()) return false;
    for (const ClassLayout* walk = cl; walk != nullptr;) {
        if (std::find(walk->accessorNames.begin(), walk->accessorNames.end(), field) !=
            walk->accessorNames.end()) {
            return false;
        }
        if (walk->superName.empty()) break;
        const ClassLayout* base = byName(walk->superName);
        if (base == walk) break;
        walk = base;
    }
    return true;
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
