// Names that resolve to nothing (docs/0027 decision 1).
//
// bronze has no global object, so identifier resolution is a closed ladder:
// this function's bindings, the enclosing environments, the module's function
// declarations, the provided globals. A name that falls off the end of it used
// to be `error: undefined variable: X`, which refuses correct programs — the
// feature-detection idiom (`typeof __THREE_DEVTOOLS__ !== 'undefined'`) and
// every browser-only function a headless run never calls.
//
// The rule this file implements instead is the spec's own, and it is not
// softer: bare `typeof` on an unresolvable name is `"undefined"` (13.5.3 step
// 1), and every other evaluation of one throws `ReferenceError: X is not
// defined` (6.2.5.5 GetValue step 2) at the moment of use. A compile-time
// WARNING names each unresolved identifier once, so the build still tells you
// about `document`; it just does not refuse the program over it.
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
    diags_.warning(span, "unresolved name '" + name +
                             "': a ReferenceError if it is evaluated (bare `typeof " + name +
                             "` is safe)");
}

// The instruction an unresolvable name lowers to. Its RESULT is a real value
// id — undefined, as `bronze_reference_error` returns — even though nothing
// can read it: the backend's exception test fires immediately after, so
// control never reaches a use, and a value id with no definition is what the
// IL verifier exists to catch.
Lowerer::Value Lowerer::emitReferenceError(const std::string& name, Span span,
                                           il::Function& ilFn) {
    // A name that IS declared and that bronze fails to bind is not an
    // unresolvable reference — it is bronze's own gap, and decision 1's rule
    // ("unprovable gets the spec's runtime behaviour") does not cover it.
    // Letting `const f = function rec(n) { return rec(n - 1) }` compile to a
    // throw would hide a compiler limitation behind a language error the
    // program could even catch.
    if (std::find(namedFunctionExprs_.begin(), namedFunctionExprs_.end(), name) !=
        namedFunctionExprs_.end()) {
        diags_.error(span, "unsupported construct: a named function expression cannot refer "
                           "to itself by name ('" + name + "')");
        // The value is never used: the caller stops at the first error. It is
        // still well-formed, so a caller that goes on to lower a sibling
        // expression does not trip the verifier before the error is reported.
        return Value{il::kNoValue, il::Type::Dynamic};
    }
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
    warnUnresolved(name, span);
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::RefError;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.keyIndex = getKeyConstantIndex(name);
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

}  // namespace bronze::lower
