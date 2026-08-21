#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "ast/ast.h"
#include "types/result.h"
#include "types/type.h"

namespace bronze::types {

// Interprocedural identity for CONSTRUCTORS: what a class constructor's
// parameters hold, joined over the construction sites this compilation can see.
//
// `method_ident.h` deliberately leaves constructors alone, and says why: a
// computed `new` names its callee at run time, and three.js has four of them.
// That refusal cost more than it saved. The library's math classes — Vector3,
// Vector2, Quaternion, Euler, Color — are written as
//
//     constructor(x = 0, y = 0, z = 0) { this.x = x; this.y = y; this.z = z; }
//
// so the write the field-type audit reads is a write of an UNTYPED PARAMETER,
// and the audit refutes `x`, `y`, `z`, `_x`, `_w`, `r`, `g`, `b` and the rest
// of the library's numeric vocabulary on that evidence alone. Every raw f64
// field load in the program is behind that one refusal.
//
// What makes a constructor different from a method, and worth the machinery:
//
//   - `new C(...)` names C. There is no dispatch to guess at, so the join over
//     the sites is a PROOF and not an optimistic identity — the same standard
//     `escapingNames` holds a top-level `function` to, and it licenses the same
//     things, `Number` and an unboxed f64 included.
//   - the sites are more than the `new`s. `super(...)` in a subclass is a call
//     of the base's constructor, and a subclass that declares NO constructor
//     gets the implicit `constructor(...args) { super(...args) }`, which
//     forwards every argument positionally. Miss either road and the join
//     speaks for callers it never saw.
//   - a DEFAULT is a call site too. `new Vec()` binds the default expression's
//     value, so `constructor(x = 0)` contributes Number whether or not any
//     `new` ever passes an argument. Without that the bare `new` would
//     contribute `undefined`, and every math class in three.js would devolve.
//
// The proof is only as good as the enumeration, so the poison is where the care
// is. A class binding read as a VALUE — stored, passed, bound, called without
// `new`, or handed to `Reflect.construct` — can be constructed by code this
// compilation cannot see, and gives up its parameters (and so do its
// ANCESTORS, because `Object.getPrototypeOf` walks from a subclass constructor
// to its base's). A class that never leaves `new` position keeps them, however
// many computed constructions the program contains.

inline constexpr uint32_t kNoCtor = 0xFFFFFFFFu;

// One class constructor as the interprocedural fixpoint sees it. Only classes
// that DECLARE a constructor are here; one that does not has no parameters of
// its own, and the arguments a `new` passes it go straight through to whatever
// its base declares (`CtorTable::targetOf`).
struct CtorInfo {
    const ast::FunctionExpr* fn = nullptr;
    std::string className;
    std::string superName;
    // No rest parameter and no destructuring pattern: the value bound is the
    // value passed, position by position. A DEFAULT is allowed, unlike the
    // method table's `plainParams`, because a default is a call site this pass
    // reads rather than a correspondence it breaks — and refusing them would
    // refuse exactly the classes this whole mechanism exists for.
    bool plainParams = true;
    // The current estimate, `Never` per parameter until a site is seen and only
    // ever widening, which is what makes the fixpoint terminate.
    Signature signature;
    std::vector<Type> observedParams;
    // Whether each parameter has a default. A site that passes no argument, or
    // passes `undefined`, binds the DEFAULT's value there, so it contributes
    // nothing of its own — the default's own type is joined in separately, by
    // the flow pass that evaluates it.
    std::vector<bool> hasDefault;
    // Parallel to the parameters: the name the field-type harvest may answer
    // for at a `this.<f> = <name>` write, or empty when it may not. Empty for a
    // pattern or rest parameter, and for a name the constructor body rebinds or
    // assigns — the harvest is syntactic and has no scope chain to tell the two
    // apart with.
    std::vector<std::string> safeParamNames;
    // `constructor(...args) { super(...args) }` — the IMPLICIT constructor,
    // which the parser synthesizes for every derived class that declares none,
    // and which a program may also write by hand. It forwards every argument
    // positionally, so it is not a constructor with parameters of its own: it is
    // a link in the chain, and `targetOf` walks straight through it.
    //
    // Reading it as an ordinary constructor is not merely imprecise, it is
    // expensive: its `super(...args)` is a SPREAD, which poisons the base — so
    // every class in three.js with a bare `class X extends Y {}` below it would
    // give up its parameters on the strength of a constructor nobody wrote.
    bool isForwarder = false;
    // No construction site this compilation saw reaches it: the signature was
    // still `Never` when the fixpoint settled.
    bool unreached = false;
};

// Every named `class` in the program, the constructor each declares, and the
// `extends` forest that decides where an argument goes when it declares none.
class CtorTable {
public:
    void build(const ast::Module& module);

    bool isClassName(const std::string& name) const {
        return classes_.find(name) != classes_.end();
    }
    uint32_t indexOfNode(const ast::FunctionExpr* fn) const;

    // The constructor a `new <className>(...)` or a `super(...)` into
    // `className` positionally reaches: the class's own, or — when it declares
    // none — whatever its base reaches, because the implicit constructor
    // forwards every argument. `kNoCtor` when nothing up the chain declares one.
    uint32_t targetOf(const std::string& className) const;

    // The constructors `new <a receiver of className>.constructor(...)` can
    // reach: the class's own target and every subclass's, since a receiver
    // typed as a base is very often an instance of something below it.
    void subtreeOf(const std::string& className, std::vector<uint32_t>& out) const;

    // `className` and every class it extends, transitively. What a poison
    // applies to: a subclass constructor that escapes as a value hands
    // `Object.getPrototypeOf` a road to its base's.
    void ancestorsOf(const std::string& className, std::vector<std::string>& out) const;

    std::vector<CtorInfo>& ctors() { return ctors_; }
    const std::vector<CtorInfo>& ctors() const { return ctors_; }

    // Parameter names the field-type harvest may answer for, per class. Built
    // once from `safeParamNames` and refreshed from the signatures each round.
    std::map<std::string, std::map<std::string, Type>> harvestOracle() const;

    // Class names the program declares more than once. Only the first is in the
    // table, so a `super(...)` written in the second would be credited to the
    // first one's base — an under-approximation, which for a proof is the fatal
    // direction. The whole mechanism stands down rather than guess which
    // declaration a body belongs to.
    const std::set<std::string>& duplicateNames() const { return duplicates_; }

private:
    struct ClassNode {
        std::string superName;
        uint32_t ctorIndex = kNoCtor;
        std::vector<std::string> children;
    };

    std::vector<CtorInfo> ctors_;
    std::map<const ast::FunctionExpr*, uint32_t> byNode_;
    std::map<std::string, ClassNode> classes_;
    std::set<std::string> duplicates_;
};

// Which classes cannot carry a parameter identity, and why. Sticky for the life
// of a compilation once set: every reason is a fact about the program text, or
// about a receiver type, and receiver types only widen.
struct CtorPoison {
    std::map<std::string, std::string> byClass;
    bool all = false;
    std::string allReason;

    bool poisons(const std::string& className) const {
        return all || byClass.find(className) != byClass.end();
    }
    // Monotone: `add` never replaces an entry and `addAll` never un-sets, so a
    // count is a version and "did the poison move this round" is a compare.
    size_t version() const { return byClass.size() + (all ? 1u : 0u); }
    const std::string& reasonFor(const std::string& className) const;
    void add(const std::string& className, const std::string& reason);
    void addAll(const std::string& reason);
};

// What the program text says about constructor VALUES, as opposed to class
// bindings. Both bits exist to answer one question: when a `new` names its
// callee at run time, which constructors can it reach?
//
// A class whose binding is read as a value is poisoned outright, so the
// interesting case is a constructor that reaches circulation WITHOUT its name
// being read — `o.constructor`, `Object.getPrototypeOf(o)`, `new.target`, or a
// computed read that could name `constructor`. If the program contains none of
// those, then every constructor an unnameable `new` can reach is one whose
// binding was read as a value, which is one already poisoned, and the site
// needs to contribute nothing at all.
struct CtorEscapeFacts {
    bool valueEscapes = false;
    std::string valueEscapeReason;
    // The program writes a name nothing in it declares (or mentions
    // `globalThis`), so a GLOBAL binding can hold whatever the program put
    // there — and `new Error(msg)` stops being a construction of a builtin.
    bool freeGlobalWrite = false;
};

// The syntactic half of the poison: everything decidable without a single type.
// The other half — a spread argument list, a `super` whose base is unknown —
// needs types and is collected by the flow pass as it runs.
void scanCtorEscapes(const ast::Module& module, const CtorTable& table, CtorPoison& poison,
                     CtorEscapeFacts& facts);

}  // namespace bronze::types
