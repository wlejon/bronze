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

    // Stamps every method-call site whose callee the module as a whole names.
    //
    // Runs once, with every function lowered, because that is the earliest
    // point at which both halves exist. It only writes `directTarget`; a site
    // it cannot resolve is the site it already was.
    void resolve(il::Module& ilModule) const;

private:
    struct Site {
        std::string receiverClass;
        std::string method;
    };
    std::unordered_map<uint32_t, Site> sites_;
    std::unordered_map<std::string, std::unordered_map<std::string, uint32_t>> classMethods_;
    std::unordered_map<std::string, std::string> classSuper_;
};

}  // namespace bronze::lower
