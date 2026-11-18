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

// Whether a WRITE to a name goes to the global object (emitUnresolvedPut):
// every name no binding of this program holds, the host's globals and the
// builtins included, and the three value properties reads fold to constants.
// The put is the global object's [[Set]] with the property's own rules: a
// read-only one (`undefined`, a host accessor with no setter such as
// `history`) is a sloppy no-op and a strict TypeError, a writable or
// replaceable one takes the value, a missing one is created in sloppy code.
bool Lowerer::writesGlobalObject(const std::string& name) const {
    if (name == "NaN" || name == "Infinity" || name == "undefined") return true;
    if (activeVarMap_.contains(name) || functionIndices_.contains(name)) return false;
    uint32_t depth = 0;
    uint32_t index = 0;
    return currentEnvValue_ == il::kNoValue || !findEnclosingEnvVar(name, depth, index);
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
    // `eval` never reaches here any more: it is a provided global now
    // (lower_util.cpp), a real function object whose body defers to the
    // host's eval hook and refuses with a catchable TypeError without one —
    // `Function`'s arrangement exactly (builtin_function.cpp,
    // rtGlobalEvalBody). The compile-time refusal that used to sit here was
    // right when bronze had no such global; a host WITH an interpreter can
    // legitimately compile the string, so the refusal became the runtime's
    // to make. The direct-call scope caveat is lowerCall's warning.
    warnUnresolved(name, span);
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::ResolveName;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.keyIndex = getKeyConstantIndex(name);
    // The identifier's own span, like every other site that can raise: it is
    // the position the ReferenceError's stack frame and the uncaught-error
    // report point at, and V8 points at the identifier too.
    inst.span = span;
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

// The WRITE of an unresolved name: 9.1.1.4.5 SetMutableBinding on the global
// environment, at run time (bronze_put_unresolved). A property the global
// object has is set; a name it lacks is created there by sloppy code
// (`Top = 1`, `Inner = 2` in a function: a configurable property a later
// `delete` removes, read back by every script through ResolveName) and is
// 6.2.5.6 PutValue step 3's ReferenceError in strict code and modules.
void Lowerer::emitUnresolvedPut(const std::string& name, Value value, Span span, il::Function& ilFn) {
    if (!resolvesName(name)) warnUnresolved(name, span);
    const uint32_t putFn = registerExternalFunction("bronze_put_unresolved", il::Type::Void,
                                                    {il::Type::Dynamic, il::Type::Dynamic, il::Type::Bool});
    il::ValueId nameVal = ilFn.valueCount++;
    il::Instruction nameInst;
    nameInst.op = il::Op::Box;
    nameInst.type = il::Type::Dynamic;
    nameInst.boxType = il::Type::Str;
    nameInst.result = nameVal;
    nameInst.keyIndex = getKeyConstantIndex(name);
    emitInst(ilFn, nameInst);
    il::ValueId strictVal = ilFn.valueCount++;
    il::Instruction boolInst;
    boolInst.op = il::Op::ConstBool;
    boolInst.type = il::Type::Bool;
    boolInst.result = strictVal;
    boolInst.immI32 = strictFlag() ? 1 : 0;
    emitInst(ilFn, boolInst);
    il::Instruction call;
    call.op = il::Op::Call;
    call.type = il::Type::Void;
    call.result = il::kNoValue;
    call.operands = {nameVal, boxValueIfNeeded(value, ilFn).id, strictVal};
    call.calleeIndex = putFn;
    call.span = span;
    emitInst(ilFn, call);
}

}  // namespace bronze::lower
