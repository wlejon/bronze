// What lowering is allowed to believe (docs/0010 decisions 3 and 5).
//
// Every question about a proven type is asked here, and here is the only
// place that knows `inference_` can be null. A null result answers
// "unproven" to everything, which reproduces the pre-inference lowering
// byte-for-byte — that is the `--no-infer` seam of decision 8, and the
// reason no other unit contains an `if (inference_)`.
//
// Nothing in this file ever *widens* what lowering does: an unproven answer
// is the uniform dynamic convention, which is always sound. A proven answer
// only ever removes boxing. There is no speculation and no deoptimization.

#include <string>
#include <vector>

#include "lower/lowerer.h"

namespace bronze::lower {

// The IL type a proven `types::Type` licenses.
//
// Only `Number` buys anything today: it is the one lattice element with
// unboxed IL ops behind it (docs/0010 decision 3). Everything else stays
// `Dynamic`, which is not a failure — it is the designed sound fallback.
//
// `Never` deliberately lands here too, and it is NOT an error. A `Never` in
// a signature is real data: it means a direct-callable function that no call
// site ever reaches, so no value ever arrives at that position (see the
// contract comment on `types::InferenceResult::signatureOf`). There is no IL
// type for "no value", and dead code still has to be lowered — it is emitted
// into the object file, it may be exported, and the IL verifier walks it —
// so it gets the uniform dynamic convention. That is the sound answer for a
// position whose callers are unknown or nonexistent; mapping `Never` onto
// f64 would be a specialization with nothing behind it, and diagnosing it
// would turn "you wrote a function nobody calls" into a compile failure.
il::Type Lowerer::ilTypeOf(types::Type t) {
    return t.is(types::TypeKind::Number) ? il::Type::F64 : il::Type::Dynamic;
}

types::Type Lowerer::inferredType(const ast::Expr& expr) const {
    if (inference_ == nullptr) return types::Type::dynamic();
    return inference_->typeAt(&expr);
}

bool Lowerer::provenNumber(const ast::Expr& expr) const {
    return inferredType(expr).is(types::TypeKind::Number);
}

// The signature of a module-level function whose calling convention
// inference actually proved, or null when it proved nothing about it.
//
// Two things have to hold before a signature is a proof:
//
//   - the function is direct-callable (docs/0010 decision 5): its name is
//     never read as a value, so every caller is a call site this
//     compilation saw and joined into the signature;
//   - the function is not exported. Decision 5's argument is "no unknown
//     callers", and an exported symbol has exactly that — a caller outside
//     this compilation, which contributed nothing to the join. `src/types`
//     does not test for export today (its escape walk only sees the AST's
//     interior), so lowering refuses the specialization rather than trust
//     a join over an incomplete set of call sites.
const types::Signature* Lowerer::provenSignature(uint32_t moduleFnIndex) const {
    if (inference_ == nullptr) return nullptr;
    if (!inference_->isDirectCallable(moduleFnIndex)) return nullptr;
    return &inference_->signatureOf(moduleFnIndex);
}

// Give a module-level function's IL skeleton the parameter and return types
// inference proved for it, so its call sites become direct *typed* calls
// (docs/0010 decision 5). Returns false only on an internal impossibility.
//
// This runs before any body is lowered, which is what makes recursion work:
// a self-call reads a signature that is already final rather than the
// half-inferred return type the first `return` statement happened to leave.
bool Lowerer::applyProvenSignature(const ast::FunctionDecl& fnDecl, uint32_t moduleFnIndex,
                                   il::Function& fn) {
    const types::Signature* sig = provenSignature(moduleFnIndex);
    if (sig == nullptr || fnDecl.isExported) return true;

    // Inference indexes module functions by their position among the
    // top-level declarations, which is the numbering assigned here. If the
    // two ever disagree, every signature is attached to the wrong function
    // — an internal impossibility, not a fallback.
    const auto byName = inference_->functionIndexOf(fnDecl.name);
    if (!byName.has_value() || *byName != moduleFnIndex ||
        sig->params.size() != fnDecl.params.size()) {
        diags_.error(fnDecl.span, "internal: inference signature for '" + fnDecl.name +
                                      "' does not match the module function table");
        return false;
    }

    // Annotations do not survive a proof. Decision 6 makes an annotation a
    // hint that can only agree with what inference proved or be discarded;
    // taking the proof here is the half of that which does not need the
    // warnings (those are step 5 of docs/0010's order of work). It also
    // closes the live unsoundness for these functions: `f(x: number)`
    // reached with a string no longer unboxes a string pointer as a double.
    const size_t base = fn.firstSourceParam();
    for (size_t i = 0; i < sig->params.size(); ++i) {
        fn.params[i + base].type = ilTypeOf(sig->params[i]);
    }

    // The return type is pinned here rather than discovered by
    // `lowerReturnStmt` from whichever `return` statement it reaches first.
    // That matters twice over: the first-return rule then unboxes every
    // later return into the first one's type (so `return 1; ... return "a"`
    // reads a string pointer as a double), and it leaves the type Void
    // while the body is being lowered, which a recursive — or mutually
    // recursive — call site reads and cannot use.
    switch (sig->returnType.kind()) {
        case types::TypeKind::Number: fn.returnType = il::Type::F64; break;
        case types::TypeKind::Bool: fn.returnType = il::Type::Bool; break;
        // Values of these kinds are boxed in the IL, so `Dynamic` is what
        // the return-statement rule would have produced anyway; saying it
        // up front is what makes the signature authoritative.
        case types::TypeKind::String:
        case types::TypeKind::Object:
        case types::TypeKind::Function:
        case types::TypeKind::Dynamic: fn.returnType = il::Type::Dynamic; break;
        // `Undefined` (every path returns nothing) keeps the Void return
        // the declaration already has, and `Never` is the dead-function
        // case described on ilTypeOf. Neither has an IL type to move to.
        case types::TypeKind::Undefined:
        case types::TypeKind::Never: break;
    }
    return true;
}

}  // namespace bronze::lower
