// What lowering is allowed to believe.
//
// Every question about a proven type is asked here, and here is the only place
// that knows `inference_` can be null. A null result answers "unproven" to
// everything, which reproduces the pre-inference lowering byte-for-byte — that
// is the `--no-infer` seam, and the reason no other unit contains an `if
// (inference_)`.
//
// Nothing in this file ever *widens* what lowering does: an unproven answer
// is the uniform dynamic convention, which is always sound. A proven answer
// only ever removes boxing. There is no speculation and no deoptimization.

// For `getenv`, which MSVC deprecates and every other toolchain does not. The
// compile-time seams are read exactly once each, at construction, from a
// single-threaded driver — the thread-safety the _s variants buy has nothing to
// hold onto here. Same reasoning, and same one-line define, as
// lower_typed_elem.cpp.
#define _CRT_SECURE_NO_WARNINGS

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "lower/lowerer.h"

namespace bronze::lower {
namespace {

// The lattice element an annotation names — the ONLY meaning an annotation has.
// A claim to be checked against a proof, never an IL type on its own.
//
// Both TypeScript's spellings and bronze's own IL names are accepted: a
// policy whose premise is "annotations are untrusted TS hints" cannot reject
// `x: string` while accepting `x: str`. Text bronze does not recognise is a
// named hard error; see `checkAnnotation` for why that is not the hint policy
// contradicting itself.
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
// Only `Number` buys anything today: it is the one lattice element with unboxed
// IL ops behind it. Everything else stays `Dynamic`, the designed sound
// fallback.
//
// `Never` lands here deliberately and is NOT an error. It means a
// direct-callable function no call site reaches, so no value ever arrives at
// that position — but dead code still has to be lowered, exported and
// verified, and there is no IL type for "no value". Mapping it onto f64 would
// be a specialization with nothing behind it; diagnosing it would turn "you
// wrote a function nobody calls" into a compile failure.
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

bool Lowerer::pristineMathCall(const ast::Expr& call) const {
    if (inference_ == nullptr) return false;
    return inference_->pristineMathCalls.count(&call) != 0;
}

// Whether a property site's receiver is proven to have ONE compile-time object
// identity, which is what licenses the inlined inline-cache check in the
// backend.
//
// The test is on the RECEIVER, not on where the property is found. A shape
// class is a claim about the object's layout, and a monomorphic receiver is
// exactly the condition an inline cache is built for: one shape reaches the
// site, so one cached entry serves it. Whether the hit is own (depth 0) or up
// the prototype chain is a fact the runtime discovers and records in the entry,
// not one the class can answer — `Foo.prototype.m` sits on a different object
// with a different shape entirely, so demanding the property be at a known
// index in the receiver's class would exclude every method call, which is the
// case three.js is made of.
//
// A receiver that joins two classes answers `Object` with no class and gets the
// plain call, so the inline form never becomes a polymorphic guard chain in
// generated code, which is a named non-goal of inference.
//
// This never removes the guard. The proof is over THIS compilation's source,
// and a shape class collects `this.x =...` assignments unconditionally,
// including ones inside branches, so a class can name a layout the runtime
// never actually builds. The emitted sequence is sound only because the shape
// word is still compared at run time; deleting the compare needs escape
// analysis, which is outside this phase.
bool Lowerer::monomorphicPropSite(const ast::Expr& receiver) const {
    const types::Type t = inferredType(receiver);
    return t.is(types::TypeKind::Object) && t.shapeClass() != types::kNoShapeClass;
}

// The IL type of a block parameter at a control-flow merge: an if/else join, a
// loop header, a loop exit.
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
// One thing has to hold: the function is direct-callable, i.e. its name is
// never read as a value, so every caller is a call site this compilation saw
// and joined into the signature.
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
// joining over all call sites, which is sound only where this compilation can
// enumerate the callers, and a closure is reached through a function value. Its
// return is a different kind of fact — one about the body alone, which the
// analysis already computes — so an annotation on it can be told apart from one
// that merely was not checkable. Nothing here changes the calling convention: a
// closure's return stays `dynamic` whatever this answers.
types::Type Lowerer::provenClosureReturn(const ast::Node& site) const {
    if (inference_ == nullptr) return types::Type::dynamic();
    return inference_->closureReturnAt(&site);
}

// A TS annotation is an untrusted optimization hint.
//
// Note what it does NOT do: it does not return a type. The caller has already
// chosen its IL type from what inference proved; this only decides whether to
// say something about the annotation.
//
//   - the proof agrees      -> silence. The typed path the caller took is
//                              taken because of the PROOF; the annotation was
//                              free information that happened to be right.
//   - nothing was proven    -> "is not provable; ignoring".
//   - something else proven -> "contradicts inferred".
//
// Both of the latter leave the value on the uniform dynamic convention. That is
// what closes the live unsoundness: believing the annotation maps `f(x:
// number)` reached with a string onto an f64 parameter and unboxes the string —
// a coercion the source never wrote, where JS says `"a" + 1` is `"a1"`.
//
// Warnings, never errors: wild JS with wrong annotations must still compile. A
// `--strict-hints` that promotes them is future work and deliberately not here.
//
// With `--no-infer` nothing is provable, so every annotation would warn and say
// only which switch is on. Those warnings are suppressed by exactly the
// `inference_ == nullptr` test that *defines* the mode, so the suppression
// cannot reach the normal one. The hard error below is NOT suppressed:
// unreadable text is a fact about the source, and a bisection seam must not
// accept a file the normal mode rejects.
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
// inference proved for it, so its call sites become direct *typed* calls.
// Returns false only on an internal impossibility.
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
    // afterwards and either says nothing or warns.
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
        case types::TypeKind::Null:
        case types::TypeKind::String:
        case types::TypeKind::Object:
        case types::TypeKind::Function:
        case types::TypeKind::TypedArray:
        case types::TypeKind::Dynamic: fn.returnType = il::Type::Dynamic; break;
        // `Undefined` (every path returns nothing) keeps the Void return
        // the declaration already has, and `Never` is the dead-function
        // case described on ilTypeOf. Neither has an IL type to move to.
        case types::TypeKind::Undefined:
        case types::TypeKind::Never: break;
    }
    return true;
}

// The syntactic form of a receiver whose type came back `Dynamic`.
//
// "receiver is dynamic" was 94 % of three.js's dynamic property sites and told
// nobody what to build: it is the analysis's TOP element, so it names an
// absence of proof rather than a mechanism that refused. This splits it by the
// expression the receiver IS, which is what a fix has to attack — `this` inside
// a method is a different problem from a parameter, and both are different from
// the result of a call.
static const char* dynamicReceiverForm(const ast::Expr& expr) {
    if (dynamic_cast<const ast::ThisExpr*>(&expr)) return "receiver is dynamic: this";
    if (dynamic_cast<const ast::Ident*>(&expr)) return "receiver is dynamic: identifier";
    if (dynamic_cast<const ast::MemberAccess*>(&expr)) return "receiver is dynamic: field read";
    if (dynamic_cast<const ast::IndexAccess*>(&expr)) return "receiver is dynamic: element read";
    if (dynamic_cast<const ast::Call*>(&expr)) return "receiver is dynamic: call result";
    if (dynamic_cast<const ast::NewExpr*>(&expr)) return "receiver is dynamic: new expression";
    if (dynamic_cast<const ast::SuperMember*>(&expr)) return "receiver is dynamic: super member";
    if (dynamic_cast<const ast::Ternary*>(&expr)) return "receiver is dynamic: ternary";
    if (dynamic_cast<const ast::Binary*>(&expr)) return "receiver is dynamic: binary";
    if (dynamic_cast<const ast::ObjectLit*>(&expr)) return "receiver is dynamic: object literal";
    if (dynamic_cast<const ast::ArrayLit*>(&expr)) return "receiver is dynamic: array literal";
    return "receiver is dynamic: other";
}

std::string Lowerer::propBailReason(const ast::Expr& expr) const {
    if (inference_ == nullptr) return "inference disabled";
    const types::Type t = inferredType(expr);
    if (t.is(types::TypeKind::Dynamic)) {
        // The identifier row is half of every remaining dynamic site, and
        // "identifier" is not a reason — it is a shape. Where the identifier
        // names a METHOD PARAMETER, the interprocedural pass knows why it could
        // not speak for it, and that is the reason worth reporting: it is what a
        // chunk aiming at these sites would have to remove.
        const auto refusal = inference_->identRefusals.find(&expr);
        if (refusal != inference_->identRefusals.end()) {
            return "receiver is dynamic: parameter (" + refusal->second + ")";
        }
        return dynamicReceiverForm(expr);
    }
    if (!t.is(types::TypeKind::Object)) {
        return std::string("receiver is ") + types::typeKindName(t.kind());
    }
    if (t.shapeClass() == types::kNoShapeClass) return "receiver shape class not proven";
    return "unknown";
}

bool Lowerer::staticShapeSeamDisabled() {
    return std::getenv("BRONZE_NO_STATIC_SHAPES") != nullptr;
}

bool Lowerer::familyGuardSeamDisabled() {
    return std::getenv("BRONZE_NO_FAMILY_GUARD") != nullptr;
}

bool Lowerer::unboxedFieldSeamDisabled() {
    return std::getenv("BRONZE_NO_UNBOXED_FIELDS") != nullptr;
}

// Is this expression a read of a field the write audit certified, on a receiver
// this compilation watched being made?
//
// The one licence for the RAW unbox and for the native arithmetic that follows
// it. Deliberately not `provenNumber`: the lattice types plenty of things
// `number` whose conversion is a CHECKED unbox and is therefore ToNumber —
// correct however the claim came about. This asks for the narrower fact, which
// is the one that carries a proof about the bits.
bool Lowerer::provenFieldRead(const ast::Expr& e) const {
    if (inference_ == nullptr || unboxedFieldsDisabled_) return false;
    return inference_->provenFieldReads.count(&e) != 0;
}

Lowerer::Value Lowerer::emitRawUnbox(Value boxed, il::Function& ilFn) {
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::Unbox;
    inst.type = il::Type::F64;
    inst.rawUnbox = true;
    inst.result = res;
    inst.operands = {boxed.id};
    emitInst(ilFn, inst);
    return Value{res, il::Type::F64};
}

// The module's proven class layouts, in the preorder the family ids number.
//
// Built on demand, from the first site that claims a family, because a program
// that proves no family should emit no table and make no registration call —
// and because the field NAMES have to be interned into the module's key pool,
// which is a lowering fact the analysis has no handle on.
void Lowerer::buildClassFamilyTable() {
    if (classFamilyTableBuilt_ || inference_ == nullptr) return;
    classFamilyTableBuilt_ = true;
    for (const types::ClassLayout* cl : inference_->classLayouts.familyPreorder()) {
        il::Module::ClassFamilyEntry entry;
        // The name is for the IL dump only; the runtime matches on the fields.
        entry.name = cl == nullptr ? std::string() : cl->name;
        if (cl != nullptr) {
            for (size_t i = 0; i < cl->fields.size(); ++i) {
                il::Module::ClassFamilyField f;
                f.keyIndex = getKeyConstantIndex(cl->fields[i]);
                f.writable = i < cl->fieldWritable.size() ? cl->fieldWritable[i] : true;
                entry.fields.push_back(f);
            }
        }
        ilModule_.classFamilies.push_back(std::move(entry));
    }
}

// Whether this property site can be compiled as a load at a constant offset.
//
// Three things have to hold, and each of them is a different kind of fact:
//
//   - the RECEIVER's class is proven (inference), and
//   - that class's LAYOUT was modellable end to end (class_layout.cpp), and
//   - the key is one of the own instance fields in it — not a prototype
//     method, not an inherited accessor, not an absent name.
//
// The third is what keeps method loads on the inline-cache path where they
// belong: `v.add` is found on `Vector3.prototype`, which is a different object
// with a different shape, so no slot of the receiver can hold it and this
// answers nullopt. That is not a limitation to remove later — a proto hit has
// no receiver slot to name, and the existing IC's depth field is the right
// mechanism for it.
std::optional<Lowerer::StaticSlotSite> Lowerer::claimStaticSlot(const ast::Expr& receiver,
                                                                const std::string& key,
                                                                bool forWrite) {
    if (staticShapesDisabled_ || inference_ == nullptr) return std::nullopt;

    // `this` inside a method of a class SOMEBODY EXTENDS is the one receiver
    // whose static class is not its runtime shape. `Object3D.updateMatrixWorld`
    // is the case that taught this: three.js never constructs a bare Object3D,
    // so every `this.matrixWorld` in that method runs on a Group or a Mesh, and
    // a cell that pins one of those shapes misses on all the others — forever,
    // at the hottest site in the scene. Chunk 6 measured the hierarchy bench 3%
    // SLOWER with these sites claimed than with the mechanism switched off, and
    // declined all 952 of them.
    //
    // The layout was never the thing that was wrong. A subclass's fields begin
    // with the base's, so slot N really is the base's field N in every subclass
    // too; what failed is the SHAPE COMPARE, an identity test that cannot be
    // loosened. So these sites get a different guard rather than a weaker one:
    // the runtime stamps each SHAPE with the most specific proven class whose
    // whole field list it verified as a prefix of that shape, and the site asks
    // whether the stamp is in its own class's `extends` subtree. Same soundness
    // story as the cell — a fact the runtime checked, so a wrong layout costs a
    // guard that never matches — and one guard now serves every subclass.
    //
    // Every other receiver this analysis types is exact by construction: the
    // type came from `new C()` or from a field whose only writes are `new C()`,
    // neither of which can produce a subclass, and those keep the cheaper
    // identity compare.
    if (dynamic_cast<const ast::ThisExpr*>(&receiver) != nullptr) {
        const types::Type t = inference_->typeAt(&receiver);
        if (t.is(types::TypeKind::Object) && inference_->classLayouts.isExtended(t.shapeClass())) {
            const uint32_t slot = inference_->staticSlotAt(&receiver, key);
            if (slot == types::ClassLayoutTable::kNoSlot) return std::nullopt;
            // The FAMILY claim, which is the one thing that changed since the
            // paragraph above was written. The layout was never the problem, so
            // the fix is not more analysis: it is a guard that asks whether the
            // shape in hand is one of the layouts that BEGIN with this class's
            // fields, which is true of every subclass by 15.7.14 and which the
            // runtime confirms name by name before it stamps the shape.
            const types::ClassLayout* fam =
                inference_->classLayouts.familyMemberOf(t.shapeClass());
            if (familyGuardDisabled_ || fam == nullptr) return std::nullopt;
            // A write needs the slot to be WRITABLE, and the stamp only stands
            // for the attributes the class declared. `Object.defineProperty(
            // this, 'id', {value: n})` declares a non-writable one, and a bare
            // store into it would silently succeed where 10.4.5 refuses.
            if (forWrite && slot < fam->fieldWritable.size() && !fam->fieldWritable[slot]) {
                return std::nullopt;
            }
            buildClassFamilyTable();
            StaticSlotSite site;
            site.slot = slot;
            site.cellIndex = 0;
            site.familyLo = fam->familyIndex;
            site.familySpan = fam->familySpan;
            return site;
        }
    }

    const uint32_t slot = inference_->staticSlotAt(&receiver, key);
    if (slot == types::ClassLayoutTable::kNoSlot) return std::nullopt;

    // An identity the interprocedural pass GUESSED does not get a CELL.
    //
    // A cell pins the first shape the site meets and misses on every other one,
    // forever. That is the right trade for a receiver whose class is exact by
    // construction — a `new C()` result, or a field whose only writes are one —
    // because one shape really does arrive. A method PARAMETER is not that: the
    // join says every caller this compilation saw passes a `Vector3`, and a
    // Vector3-typed parameter still receives whichever of its subclasses, or
    // whichever shape a late property add produced, the program actually built.
    //
    // Measured, on brobench, with the cell allowed here: many_meshes 37.8 vs
    // 36.4 ms, instanced 16.0 vs 15.5, hierarchy 5.6 vs 5.3 — a 3-4% REGRESSION
    // across all three scenes, two interleaved passes agreeing. Same shape as the
    // 952 sites chunk 6 gave back, and the same cause. So a guessed identity may
    // claim a slot only where the guard tolerates a family of shapes, and takes
    // the ordinary inline cache otherwise.
    const types::Type recvType = inference_->typeAt(&receiver);
    if (recvType.identityOnly()) {
        const types::ClassLayout* fam =
            inference_->classLayouts.familyMemberOf(recvType.shapeClass());
        if (familyGuardDisabled_ || fam == nullptr) return std::nullopt;
        if (forWrite && slot < fam->fieldWritable.size() && !fam->fieldWritable[slot]) {
            return std::nullopt;
        }
        buildClassFamilyTable();
        StaticSlotSite site;
        site.slot = slot;
        site.cellIndex = 0;
        site.familyLo = fam->familyIndex;
        site.familySpan = fam->familySpan;
        return site;
    }
    return StaticSlotSite{slot, staticSiteCounter_++};
}

void Lowerer::stampStaticSlot(il::Instruction& inst, const ast::Expr& receiver) {
    if (inst.keyIndex >= keyStrings_.size()) return;
    const auto site = claimStaticSlot(receiver, keyStrings_[inst.keyIndex],
                                      inst.op == il::Op::PropSet);
    if (!site) return;
    inst.staticSlot = site->slot;
    inst.staticCellIndex = site->cellIndex;
    inst.familyLo = site->familyLo;
    inst.familySpan = site->familySpan;
    if (stats_) {
        stats_->recordStaticSlot(receiver.span.file,
                                site->familyLo != il::Instruction::kNoFamily);
    }
}

void Lowerer::reportClassLayouts() {
    if (stats_ == nullptr || inference_ == nullptr) return;
    uint32_t proven = 0;
    uint32_t familyRoots = 0;
    for (const auto& cl : inference_->classLayouts.all()) {
        if (cl.layoutProven) ++proven;
        // A root of the forest is a family member whose base is not one: either
        // it has no `extends`, or its base's layout was refused, or its base
        // declares no fields of its own.
        if (cl.familyIndex == types::ClassLayout::kNoFamily) continue;
        const types::ClassLayout* base = inference_->classLayouts.byName(cl.superName);
        if (base == nullptr || base->familyIndex == types::ClassLayout::kNoFamily) ++familyRoots;
    }
    stats_->recordClassLayouts(
        proven, static_cast<uint32_t>(inference_->classLayouts.familyPreorder().size()),
        familyRoots, inference_->classLayouts.refusalHistogram());
    stats_->recordFieldAudit(inference_->fieldAudit,
                             static_cast<uint32_t>(inference_->provenFieldReads.size()));
}

void Lowerer::recordPropertyAccess(uint16_t fileId, bool isNative,
                                   const std::string& bailReason) {
    if (stats_) stats_->recordPropertyAccess(fileId, isNative, bailReason);
}

void Lowerer::recordCall(uint16_t fileId, bool isNative,
                         const std::string& bailReason) {
    if (stats_) stats_->recordCall(fileId, isNative, bailReason);
}

void Lowerer::recordElementOp(uint16_t fileId, bool isNative,
                              const std::string& bailReason) {
    if (stats_) stats_->recordElementOp(fileId, isNative, bailReason);
}

}  // namespace bronze::lower
