#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "ast/ast.h"
#include "types/class_layout.h"
#include "types/result.h"
#include "types/type.h"

namespace bronze::types {

// Interprocedural identity: what a class method's PARAMETERS hold, joined over
// the call sites this compilation can see.
//
// The module-level call-graph fixpoint in infer.cpp has always done this for
// top-level `function` declarations, whose callers are enumerable because the
// name never escapes. A method's callers are not enumerable that way — a method
// is reached through a value, `o.m`, and `o` can be anything. But three.js is
// not written in top-level functions; it is 200 classes, and the receivers its
// methods run on are overwhelmingly ones the analysis already types (`this`, a
// field whose only writes are `new C()`, a local bound to a `new`). So the
// callers ARE mostly enumerable — through the RECEIVER's class rather than
// through the callee's name.
//
// That is what this table is: for each `class` method, the join of the argument
// types at every call `<receiver of class C>.m(...)` that could reach it. The
// answer is a `Type::objectIdentityOnly` — see the long note there for why a
// guess is worth having and what it is forbidden to be spent on.
//
// Two things are NOT modelled here, deliberately:
//
//   - constructors. `new C(...)` sites are precise, but a computed `new` —
//     three.js has four, `new Curves[type]()` and friends — reaches a
//     constructor whose name is data, so every constructor in the program would
//     have to give up its parameters for them. The result of a `new` is already
//     typed exactly; its arguments are mostly numbers; the trade is not there.
//
//   - methods installed on a prototype by assignment or `Object.assign`. Those
//     are function expressions, reached the same way any other value is, and a
//     class body is what gives a method a receiver whose class is knowable.
//     A class method that such an assignment later OVERWRITES simply becomes
//     dead code whose parameters were typed; nothing reads them.

inline constexpr uint32_t kNoMethod = 0xFFFFFFFFu;

// One class method as the interprocedural fixpoint sees it.
struct MethodInfo {
    const ast::FunctionExpr* fn = nullptr;
    std::string className;
    std::string methodName;
    // No default, rest or destructured parameter: the value bound is the value
    // passed, position by position, which is what a joined signature IS.
    bool plainParams = true;
    // The current estimate, `Never` per parameter until a call site is seen and
    // only ever widening — the same discipline `FunctionInfo::signature` keeps,
    // and the reason the fixpoint terminates.
    Signature signature;
    std::vector<Type> observedParams;
    Type observedReturn = Type::never();
    // No call site this compilation saw reaches it: the signature was still
    // `Never` when the fixpoint settled. Its parameters then take the uniform
    // dynamic convention like anyone else's, and this records which of the two
    // reasons that was — so the stats row can tell "nobody calls it" from "the
    // callers disagree".
    bool unreached = false;
};

// Every `class` method in the program, and the `extends` forest that decides
// which of them a call on a receiver of a given class can reach.
class MethodTable {
public:
    void build(const ast::Module& module);

    uint32_t indexOfNode(const ast::FunctionExpr* fn) const;

    // Whether any class in the program declares a method by this name. The
    // escape scan asks it before poisoning a name: `this.position` is a member
    // read of a name no class declares a method under, and a poison list full of
    // field names would say nothing.
    bool isMethodName(const std::string& name) const;

    // Every method a call `recv.name(...)` can reach when `recv` is known to be
    // an instance of `className`: the nearest declaration at or above it, plus
    // every override in its subtree.
    //
    // The subtree is not paranoia. A receiver typed `Object3D` is very often a
    // `Mesh` — `Scope::thisClass` in a base method names the class that DECLARED
    // the method, not the class that was constructed, and a field whose writes
    // the analysis harvested from one class body can still be assigned a
    // subclass instance from another. So a call on an `Object3D` contributes to
    // `Mesh::raycast` as well as to `Object3D::raycast`.
    void reachableFrom(const std::string& className, const std::string& methodName,
                       std::vector<uint32_t>& out) const;

    // Every declaration of `name`, anywhere. What a poison applies to.
    const std::vector<uint32_t>* declarationsOf(const std::string& name) const;

    std::vector<MethodInfo>& methods() { return methods_; }
    const std::vector<MethodInfo>& methods() const { return methods_; }

private:
    struct ClassNode {
        std::string superName;
        std::map<std::string, uint32_t> ownMethods;  // name -> method index
        std::vector<std::string> children;
    };

    std::vector<MethodInfo> methods_;
    std::map<const ast::FunctionExpr*, uint32_t> byNode_;
    std::map<std::string, std::vector<uint32_t>> byName_;
    std::map<std::string, ClassNode> classes_;
};

// Why a method's parameters cannot carry an identity. Sticky for the life of a
// compilation once set: every reason below is a fact about the program text or
// about a receiver type, and receiver types only widen.
struct MethodPoison {
    // Names no method may speak for, and why. A name, not a method: a call the
    // analysis cannot resolve names a METHOD NAME, and any class's declaration
    // of it could be the one that runs.
    std::map<std::string, std::string> byName;
    // A computed call whose property name could not be pinned to a literal set
    // reaches every method in the program.
    bool all = false;
    std::string allReason;

    bool poisons(const std::string& name) const {
        return all || byName.find(name) != byName.end();
    }
    // Monotone: `add` never replaces an entry and `addAll` never un-sets. So a
    // count is a version, and "did the poison move on this round" is a compare.
    size_t version() const { return byName.size() + (all ? 1u : 0u); }
    const std::string& reasonFor(const std::string& name) const;
    void add(const std::string& name, const std::string& reason);
    void addAll(const std::string& reason);
};

// The purely SYNTACTIC half of the poison: everything decidable without a
// single type. A method name read anywhere but in the callee position of a call
// escapes as a value; `.call`/`.apply`/`.bind` route a call past the receiver
// this analysis reasons about; a computed call names its method at run time.
//
// The other half — a call whose receiver has no proven class — needs types and
// is collected by the flow pass as it runs (`ModuleContext::methodPoison`).
void scanMethodEscapes(const ast::Module& module, const MethodTable& table,
                       MethodPoison& out);

}  // namespace bronze::types
