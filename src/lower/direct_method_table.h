// The DIRECT METHOD-CALL EDGE (il.h `directTarget`): the two-sided table that
// records which function a `recv.m(...)` site names, and the pass that matches
// the two halves once the module is whole.
//
// Its own component and not a group of `Lowerer` members because it needs
// nothing from lowering: names in, names in, an il::Module out. Everything
// about the RECEIVER that inference has to answer is resolved by the caller
// and arrives here as a class name.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "il/il.h"

namespace bronze::lower {

// Naming a method-call site's callee is a two-sided fact and the two sides
// are lowered in the wrong order: a top-level function's body is lowered
// BEFORE `main`, and a class body is evaluated INSIDE `main`. So
// `c.multiplyMatrices(a, b)` in `run` is lowered while
// `Matrix4.prototype.multiplyMatrices` is not yet a function in the module
// at all. Both sides therefore record what they know, keyed on names, and
// `resolve` matches them once the module is whole.
class DirectMethodTable {
public:
    // Keyed on the site's IC INDEX rather than on a (function, block,
    // instruction) triple: the index is already unique across the module and
    // already on the instruction, so the resolver needs no second numbering to
    // stay in step with.
    //
    // `receiverClass` is the class lowering believes the receiver has, or
    // empty. The nearest declaration at or above it is the callee; a subclass
    // override below it is exactly what the backend's code-pointer compare
    // rejects.
    void recordSite(uint32_t icIndex, std::string receiverClass, std::string method);
    // One entry of class name -> method name -> module function index, for the
    // ordinary instance methods (not static, not private, not an accessor, and
    // not a computed key: none of those is reachable as `recv.<name>`).
    void recordClassMethod(const std::string& className, const std::string& superName,
                           const std::string& method, uint32_t fnIndex);
    // The name a class extends, so the resolver can walk to the nearest
    // declaration without asking inference a second time.
    void recordClassSuper(const std::string& className, const std::string& superName);

    // The ACCESSOR halves of the same edge (il.h `directTarget` on
    // PropGet/PropSet): a class's instance `get key()` / `set key(v)`, and a
    // named property read or write whose receiver lowering believes has
    // `receiverClass`. Keyed on the site's IC index like a method call's.
    void recordClassAccessor(const std::string& className, const std::string& key, bool setter,
                             uint32_t fnIndex);
    void recordAccessorSite(uint32_t icIndex, std::string receiverClass, std::string key);

    // Stamps every method-call site whose callee the module as a whole names.
    //
    // Runs once, with every function lowered, because that is the earliest
    // point at which both halves exist. It only writes `directTarget`; a site
    // it cannot resolve is the site it already was.
    void resolve(il::Module& ilModule) const;

    // Stamps every property read and write whose receiver's class declares
    // (at or above it, with no method of that name in between) an accessor
    // for its key: `directTarget` the getter or setter, and `staticCellIndex`
    // a fresh pair of static cells (its guard, runtime/accessor_cell.cpp),
    // grown onto the module's `staticSiteCount`. Every instruction of one IC
    // site shares one pair, so the copies the IL -> IL passes made of a site
    // share its cell as they share its cache.
    //
    // Runs after those passes and before guarded inlining, which copies the
    // accessor's body in behind the cell's guard: the stamp is a fact only
    // about the finished sites, and nothing before inlining reads it.
    void resolveAccessors(il::Module& ilModule) const;

    // Marks every method call whose key names an instance method of some
    // class here AND is the key of some named property write in the module
    // (il.h `kMethodOwnCallee`): the sites where a receiver may hold its
    // callee in an own slot, shadowing the prototype's. Keys only; whether a
    // receiver in hand does is the method cache's SLOT entry's to say.
    void markOwnCalleeSites(il::Module& ilModule) const;

    // Returns true when `icIndex` is a recorded method-call site with a known
    // non-ambiguous receiver class hierarchy and no strict subclass in the
    // module overrides the method.
    bool isMethodSealedOnSubclasses(uint32_t icIndex) const;

private:
    struct Site {
        std::string receiverClass;
        std::string method;
    };
    std::unordered_map<uint32_t, Site> sites_;
    std::unordered_map<uint32_t, Site> accessorSites_;
    // class -> key -> (getter, setter) function indices, kNoDirectTarget for
    // a half the class does not declare.
    struct AccessorPair {
        uint32_t getter = il::Instruction::kNoDirectTarget;
        uint32_t setter = il::Instruction::kNoDirectTarget;
    };
    std::unordered_map<std::string, std::unordered_map<std::string, AccessorPair>> classAccessors_;
    std::unordered_map<std::string, std::unordered_map<std::string, uint32_t>> classMethods_;
    std::unordered_map<std::string, std::string> classSuper_;
    std::unordered_set<std::string> declaredClasses_;
    std::unordered_set<std::string> ambiguousClasses_;
};

}  // namespace bronze::lower
