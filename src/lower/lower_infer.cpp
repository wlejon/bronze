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

#include <optional>
#include <string>
#include <vector>

#include "lower/lowerer.h"

namespace bronze::lower {
namespace {

// The lattice element an annotation names — the ONLY meaning an annotation
// has (docs/0010 decision 6). It is a claim to be checked against a proof,
// never an IL type, which is why this replaced the old `mapTypeAnnotation`:
// that function answered "what IL type does this annotation buy", and the
// answer is now "none, on its own".
//
// The spellings are TypeScript's, because that is what the hint policy is
// about: an annotation is an untrusted TS hint, so `x: string` has to be
// readable and `x: str` was only ever readable because `str` is what bronze
// happens to call the type in its own IL. Both are accepted — the IL names
// stay because they were accepted before and a ratchet only grows — but a
// user writing TS never has to learn them.
//
// Text bronze does not recognise stays a named hard error; see
// `checkAnnotation` for why that is not the hint policy contradicting
// itself.
std::optional<types::Type> annotationClaim(const std::string& ann) {
    // TS spelling first in each line, bronze's IL name after it.
    if (ann == "number" || ann == "f64" || ann == "i32") return types::Type::number();
    if (ann == "boolean" || ann == "bool") return types::Type::boolean();
    if (ann == "string" || ann == "str") return types::Type::string();
    // TS `void` is "returns nothing", which is bronze's Undefined.
    if (ann == "undefined" || ann == "void") return types::Type::undefined();
    if (ann == "null") return types::Type::null();
    if (ann == "object") return types::Type::object();
    if (ann == "never") return types::Type::never();
    // The top type under three names. It claims nothing, so nothing can
    // disagree with it (see `checkAnnotation`).
    if (ann == "any" || ann == "unknown" || ann == "dynamic") return types::Type::dynamic();
    return std::nullopt;
}

// Every spelling above, for the error that rejects anything else — the fix
// for `x: Widget` is not discoverable otherwise.
constexpr const char* kReadableAnnotations =
    "any, bool, boolean, dynamic, f64, i32, never, null, number, object, str, string, "
    "undefined, unknown, void";

}  // namespace

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

// Whether a property site's receiver is proven to have ONE compile-time
// object identity, which is what licenses the inlined inline-cache check in
// the backend (docs/0010 decisions 4 and 7).
//
// The test is on the RECEIVER, not on where the property is found. A shape
// class is a claim about the object's layout, and a monomorphic receiver is
// exactly the condition an inline cache is built for: one shape reaches the
// site, so one cached entry serves it. Whether the hit is own (depth 0) or
// up the prototype chain is a fact the runtime discovers and records in the
// entry, not one the class can answer — docs/0008 puts `Foo.prototype.m`
// on a different object with a different shape entirely, so demanding the
// property be at a known index in the receiver's class would exclude every
// method call, which is the case three.js is made of.
//
// A receiver that joins two classes answers `Object` with no class and gets
// the plain call, so the inline form never becomes a polymorphic guard
// chain in generated code — the named non-goal of decision 4.
//
// This never removes the guard. The proof is over THIS compilation's
// source, and a shape class collects `this.x = ...` assignments
// unconditionally, including ones inside branches, so a class can name a
// layout the runtime never actually builds. The emitted sequence is sound
// only because the shape word is still compared at run time; deleting the
// compare needs escape analysis, which docs/0010 places outside this phase.
bool Lowerer::monomorphicPropSite(const ast::Expr& receiver) const {
    const types::Type t = inferredType(receiver);
    return t.is(types::TypeKind::Object) && t.shapeClass() != types::kNoShapeClass;
}

// The IL type of a block parameter at a control-flow merge (docs/0005
// decision 2): an if/else join, a loop header, a loop exit.
//
// A parameter's type has to be an upper bound of EVERY edge into its block,
// and lowering does not have those edges in hand — a loop header is typed
// before its back edge is lowered. Only the flow analysis knows what the
// back edge can carry (`typeOfBindingAt`), so this is the one source, and
// with no inference the answer is `Dynamic` for everything. That is the
// point: the pre-inference behaviour of taking the type from whatever value
// happened to be live at loop *entry* is not a conservative fallback, it is
// a claim that the loop cannot change the binding's type, and a loop that
// does was miscompiled into unboxing a string as a double.
//
// `Bool` is admitted here, unlike `ilTypeOf`, which is about a calling
// convention. A boxed value coerced to a `bool` parameter goes through
// `unbox.bool`, which is JS ToBoolean — lossy for anything that is not
// already a boolean, and *exact* for one. So the coercion is faithful
// precisely when the analysis proved `Bool`, which is the only case that
// reaches it.
il::Type Lowerer::mergeParamType(const ast::Stmt& mergePoint, const std::string& name) const {
    if (inference_ == nullptr) return il::Type::Dynamic;
    switch (inference_->typeOfBindingAt(&mergePoint, name).kind()) {
        case types::TypeKind::Number: return il::Type::F64;
        case types::TypeKind::Bool: return il::Type::Bool;
        default: return il::Type::Dynamic;
    }
}

// The signature of a module-level function whose calling convention
// inference actually proved, or null when it proved nothing about it.
//
// One thing has to hold: the function is direct-callable (docs/0010
// decision 5), i.e. its name is never read as a value, so every caller is a
// call site this compilation saw and joined into the signature.
//
// `export` is deliberately NOT re-tested here. An exported function has a
// caller outside this compilation and must never be specialized, but that
// is a fact about the escape set, so `types::escapingNames` is where it is
// decided; a second copy of the rule in lowering is a copy that can drift.
const types::Signature* Lowerer::provenSignature(uint32_t moduleFnIndex) const {
    if (inference_ == nullptr) return nullptr;
    if (!inference_->isDirectCallable(moduleFnIndex)) return nullptr;
    return &inference_->signatureOf(moduleFnIndex);
}

// What inference proved about one position of a module-level function's
// calling convention, for the annotation check to compare against. No proof
// — no inference result, an escaping or exported function, an index out of
// range — answers `Dynamic`, which is the honest report of "nothing was
// observed here" and is exactly what the warning then says.
types::Type Lowerer::provenParamType(uint32_t moduleFnIndex, size_t paramIndex) const {
    const types::Signature* sig = provenSignature(moduleFnIndex);
    if (sig == nullptr || paramIndex >= sig->params.size()) return types::Type::dynamic();
    return sig->params[paramIndex];
}

types::Type Lowerer::provenReturnType(uint32_t moduleFnIndex) const {
    const types::Signature* sig = provenSignature(moduleFnIndex);
    return sig == nullptr ? types::Type::dynamic() : sig->returnType;
}

// What a closure's body was observed to return. A closure has no module
// function index, so `provenSignature` cannot speak for it; the AST node is
// its only handle (`types::InferenceResult::closureReturnAt`).
//
// This is the whole of a closure's proof surface, and deliberately so. Its
// PARAMETERS have no proof and cannot get one: a signature is inferred by
// joining over all call sites (docs/0010 decision 5), which is sound only
// where this compilation can enumerate the callers, and a closure is reached
// through a function value. Its return is a different kind of fact — one
// about the body alone, which the analysis already computes — so an
// annotation on it can be told apart from one that merely was not checkable.
// Nothing here changes the calling convention: a closure's return stays
// `dynamic` whatever this answers.
types::Type Lowerer::provenClosureReturn(const ast::Node& site) const {
    if (inference_ == nullptr) return types::Type::dynamic();
    return inference_->closureReturnAt(&site);
}

// docs/0010 decision 6, and with it docs/0001 decision 4: a TS annotation is
// an untrusted optimization hint.
//
// This function is the whole policy, and note what it does NOT do — it does
// not return a type. An annotation seeds nothing here and constrains nothing;
// the caller has already chosen its IL type from what inference proved, and
// this only decides whether to say something about the annotation:
//
//   - the proof agrees      -> silence. The typed path the caller took is
//                              taken because of the PROOF; the annotation was
//                              free information that happened to be right.
//   - nothing was proven    -> "is not provable; ignoring".
//   - something else proven -> "contradicts inferred".
//
// Both of the latter leave the value on the uniform dynamic convention. That
// is what closes the live unsoundness docs/0010 named: `f(x: number)` reached
// with a string used to map straight onto an f64 parameter and unbox the
// string, which is a coercion the source never wrote — JS `"a" + 1` is `"a1"`.
//
// Warnings, never errors: wild JS with wrong annotations must still compile
// (docs/0001 decision 4). A `--strict-hints` that promotes them is named as
// future work in docs/0010 and deliberately not here.
//
// With `--no-infer` there is no proof for ANY annotation to agree with, so
// every one of them would be discarded and every one would warn. Those
// warnings are suppressed, because they say nothing about the source — only
// about the mode, which the user chose one command line ago. The suppression
// is exactly the `inference_ == nullptr` test that *defines* the mode, so it
// cannot hide a warning in the normal mode: with an inference result in hand
// every discarded annotation still warns, and that is what the lower tests
// pin in both directions.
//
// What is NOT suppressed is the hard error below. Unreadable annotation text
// is a fact about the source, and `--no-infer` is a bisection seam for a
// suspected miscompile (docs/0010 decision 8) — it must not quietly accept
// source that the normal mode rejects.
bool Lowerer::checkAnnotation(const std::string& ann, Span span, const std::string& name,
                              types::Type proven) {
    if (ann.empty()) return true;
    const auto claim = annotationClaim(ann);
    if (!claim) {
        // Kept a hard error, not turned into another discarded hint. The
        // hint policy is about TRUST — an annotation never types anything —
        // not about readability, and text bronze cannot read is not an
        // over-optimistic hint but a construct it has no lattice element for
        // (a nominal type, a generic, an interface). The house rule is that
        // such a construct is diagnosed by name; ignoring it silently is the
        // quiet no-op the rules forbid. The vocabulary is listed so the fix
        // is visible from the message.
        diags_.error(span, "unsupported type annotation: " + ann +
                               " (bronze reads: " + kReadableAnnotations + ")");
        return false;
    }
    // `any`/`unknown`/`dynamic` claims nothing about the value, so no proof
    // can disagree with it and there is nothing to discard.
    if (claim->is(types::TypeKind::Dynamic)) return true;
    // By KIND, not by whole type: an annotation can never carry an identity
    // (there is no source syntax for a shape class), so `object` agreeing
    // with a proven `object#3` is agreement, and comparing the payloads
    // would report "annotation 'object' contradicts inferred object".
    if (claim->kind() == proven.kind()) return true;
    if (inference_ == nullptr) return true;

    // The kind name, not `Type::str()`: a shape class id (`object#3`) is a
    // compile-time identity with no meaning in the source the user wrote.
    const std::string observed = types::typeKindName(proven.kind());
    // `Never` is "no value ever arrives here" (a direct-callable function
    // with no call sites), which is an absence of evidence rather than
    // evidence against — so it is not provable, not a contradiction.
    if (proven.is(types::TypeKind::Dynamic) || proven.is(types::TypeKind::Never)) {
        diags_.warning(span, "annotation '" + ann + "' on '" + name +
                                 "' is not provable; ignoring (inferred: " + observed + ")");
    } else {
        diags_.warning(span, "annotation '" + ann + "' on '" + name +
                                 "' contradicts inferred " + observed);
    }
    return true;
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
    // No `fnDecl.isExported` test here: an exported function is not
    // direct-callable in the first place, because `types::escapingNames`
    // puts its name in the escape set. Do not re-add the guard — it would
    // be a second copy of a rule that belongs to the analysis.
    const types::Signature* sig = provenSignature(moduleFnIndex);
    if (sig == nullptr) return true;

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

    // The proof, and nothing else, types the parameters. An annotation never
    // reaches this loop — `checkAnnotation` compares it to the same proof
    // afterwards and either says nothing or warns (docs/0010 decision 6).
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
