// Names COMPILE-TIME resolution cannot settle.
//
// Identifier resolution here is a closed ladder: this function's bindings, the
// enclosing environments, the module's function declarations, the provided
// globals. A name that falls off the end of it used to be `error: undefined
// variable: X`, which refuses correct programs — the feature-detection idiom
// (`typeof __THREE_DEVTOOLS__ !== 'undefined'`) and every browser-only
// function a headless run never calls.
//
// What falls off the end is not unresolvable, only unresolved YET. 9.1.1.4
// makes the global environment's object record `globalThis`, so a property of
// that object is a global binding and `globalThis.navigator = {}` creates one
// the next free `navigator` reads — which no pass here can see. So the name
// travels to run time as `name.resolve`, where the object is asked and a miss
// is 6.2.5.5 GetValue step 2's `ReferenceError: X is not defined` at the
// moment of use. Bare `typeof` asks the same question and answers "undefined"
// for a miss instead of throwing (13.5.3 step 1).
//
// A compile-time WARNING names each unresolved identifier once, so the build
// still tells you about `document`; it just does not refuse the program over
// it.
//
// The line this does NOT cross is a member of something bronze knows:
// `console.table` stays a compile error by name, dead code and `typeof`
// included, because bronze knows what `console` has. Provable is diagnosed
// now; unprovable gets the spec's runtime behaviour and a warning.

#include <algorithm>
#include <string>

#include "lower/lowerer.h"

namespace bronze::lower {

// The resolution ladder of `lowerExpr`'s identifier path, asked as a question
// rather than performed. It exists so `typeof x` can find out whether `x`
// resolves WITHOUT emitting the instructions a read would; keeping the two in
// one file is what stops them drifting into disagreement about what "free"
// means.
bool Lowerer::resolvesName(const std::string& name) const {
    if (activeVarMap_.contains(name)) return true;
    // The three global value properties lowering folds to constants. They are
    // resolvable names, not provided globals, because nothing looks them up.
    if (name == "NaN" || name == "Infinity" || name == "undefined") return true;
    uint32_t depth = 0;
    uint32_t index = 0;
    if (currentEnvValue_ != il::kNoValue && findEnclosingEnvVar(name, depth, index)) return true;
    if (functionIndices_.contains(name)) return true;
    return isProvidedGlobal(name);
}

// Once per NAME, not once per mention: `document` appears eleven times in
// three.js's utils.js and eleven identical warnings is a diagnostic nobody
// reads. The set is per-module and lowering order is deterministic, so the
// warning stream is too.
void Lowerer::warnUnresolved(const std::string& name, Span span) {
    if (!warnedUnresolved_.insert(name).second) return;
    // A folded console member is not a missing global: the parser folded it
    // knowing what it is, and the warning has to say so or the reader goes
    // looking for a binding that was never the problem.
    if (name.rfind("console.", 0) == 0) {
        diags_.warning(span, "unsupported: " + name +
                                 " is not implemented (console provides log, info, debug, warn "
                                 "and error); a ReferenceError if it is evaluated");
        return;
    }
    diags_.warning(span, "unresolved name '" + name +
                             "': resolved against the global object at run time, and a "
                             "ReferenceError if it is not there either (bare `typeof " + name +
                             "` is safe)");
}

// The instruction an unresolved name lowers to. Its result is READ: the helper
// hands back the global object's own property when the program made one, and
// only raises when it did not — so the backend's exception test after it may
// or may not fire, where before it always did.
Lowerer::Value Lowerer::emitReferenceError(const std::string& name, Span span,
                                           il::Function& ilFn) {
    // A `var` written inside a block of this function. 8.6.2 hoists it to the
    // function whatever depth it sits at; bronze creates the slot only for the
    // ones written at the function's top level, so a read of any other one
    // arrives here having resolved to nothing. It is the same class of thing as
    // the named function expression above — declared, and bronze's own gap —
    // and it had been passing through as `unresolved name 'j'`, which sends the
    // reader looking for a missing global instead of at the hoisting.
    if (std::find(functionVarNames_.begin(), functionVarNames_.end(), name) !=
        functionVarNames_.end()) {
        diags_.error(span, "unsupported construct: '" + name +
                               "' is declared by a `var` inside a block, and bronze hoists "
                               "`var` only from a function's top level");
        return Value{il::kNoValue, il::Type::Dynamic};
    }
    // `eval` is not a missing global — it is a global bronze will never have.
    // 19.2.1 makes it a function of the global object whose argument is SOURCE
    // TEXT compiled at run time, and bronze compiles ahead of time and links no
    // compiler into the program; there is nothing for it to resolve to and
    // nothing a host could register that would be it.
    //
    // Diagnosed here, at the READ, so both spellings land: `eval(src)` is a
    // direct eval and `const e = eval; e(src)` is an indirect one, and both
    // read the name. Bare `typeof eval` does not reach here at all — the
    // `typeof` path in lowerExpr answers for any name `resolvesName` rejects —
    // so the feature-detection idiom still compiles, and the diagnostic fires
    // exactly when a program means to USE the thing.
    //
    // `Function` is the sibling case and is deliberately NOT here: it is a
    // provided global with a real constructor object, so `Function.prototype`
    // and `f.call` work and only CONSTRUCTING from source text fails — a
    // catchable TypeError from `functionConstructorBody`, since `new Function`
    // is a call bronze cannot see through the way it sees a spelled-out `eval`.
    if (name == "eval") {
        diags_.error(span,
                     "unsupported construct: `eval` is not implemented (bronze compiles ahead "
                     "of time and links no compiler into the program; this covers indirect eval "
                     "too, and `new Function(src)` throws a TypeError for the same reason)");
        return Value{il::kNoValue, il::Type::Dynamic};
    }
    warnUnresolved(name, span);
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::ResolveName;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.keyIndex = getKeyConstantIndex(name);
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

}  // namespace bronze::lower
