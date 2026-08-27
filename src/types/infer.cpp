
#include "types/infer.h"

#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "ast/queries.h"
#include "ast/query_walk.h"
#include "types/ctor_ident.h"
#include "types/escape.h"
#include "types/flow.h"
#include "types/method_ident.h"

namespace bronze::types {
namespace {

// The module top level is a function body like any other, and lowering emits
// it as one. Naming it here keeps the dump and the IL agreeing about which
// code is which.
constexpr const char* kTopLevelName = "main";

struct ModuleSplit {
    std::vector<const ast::FunctionDecl*> decls;  // module functions, source order
    std::vector<const ast::Stmt*> topLevel;       // everything else, source order
};

// Splits exactly as lowering does: a top-level function declaration becomes a
// module function, indexed by its position among them, and the rest is
// `main`. The index has to match, because it is what `signatureOf` is keyed
// on and what a direct call will name.
ModuleSplit splitModule(const ast::Module& module) {
    ModuleSplit split;
    for (const auto& stmt : module.body) {
        if (!stmt) continue;
        if (const auto* fn = dynamic_cast<const ast::FunctionDecl*>(stmt.get())) {
            split.decls.push_back(fn);
        } else {
            split.topLevel.push_back(stmt.get());
        }
    }
    return split;
}

// The module top level, walked on its own. Split out because the fixpoint has
// to walk it BEFORE its first round as well as inside every round — see
// `seedModuleBindings`.
bool runTopLevel(ModuleContext& mod, const ModuleSplit& split, bool record) {
    if (split.topLevel.empty()) return true;
    Span span{};
    span.begin = split.topLevel.front()->span.begin;
    span.end = split.topLevel.back()->span.end;

    FunctionAnalysisArgs args;
    args.parent = nullptr;
    args.qualifiedName = kTopLevelName;
    args.moduleIndex = kNoFunctionIndex;
    args.site = nullptr;
    args.directCallable = false;
    args.body = split.topLevel;
    args.span = span;
    args.record = record;
    args.moduleTopLevel = true;

    return analyzeFunction(mod, args).ok;
}

// What `const _m1 = new Matrix4()` holds, decided before the first round reads
// it (flow.h `moduleBindings`).
//
// The table is filled at the END of the top level's walk, and every round walks
// the bodies first — so on round one every module-scope receiver in every
// method body answers `Dynamic`, and `_m1.copy( this )` is a call whose class
// the pass cannot name. Such a call contributes its arguments to EVERY method
// of that name (flow_expr.cpp), which is how a `Vector3` reaches
// `Matrix4.copy`'s parameter; and because the signature fold only ever WIDENS,
// the two classes join to an object with no class and the later rounds — which
// do resolve `_m1` — can never take it back. three.js is made of module-scope
// temporaries and of method names four classes share, so this was most of the
// library's shared vocabulary giving up its parameters on round one.
//
// One extra walk of the top level, not recording and with its observations
// discarded by the `resetObservations` at the head of the loop. The table is
// joined into and never rebuilt, so seeding it early can only make round one
// answer what a later round would have answered anyway.
void seedModuleBindings(ModuleContext& mod, const ModuleSplit& split) {
    if (!mod.valueFlow) return;
    // The outcome is deliberately not consulted: a walk that fails has set
    // `mod.failed`, and round one walks the same statements and reports it —
    // this one is an extra look at a body the loop reads anyway, and a second
    // road out of the function here would be a second place to keep right.
    runTopLevel(mod, split, /*record=*/false);
}

// One walk of every function in the module, plus the top level. Call sites
// discovered here widen the callee's `observedParams`, which is what the
// outer fixpoint folds back into the signatures.
bool runPass(ModuleContext& mod, const ModuleSplit& split, bool record) {
    for (uint32_t i = 0; i < mod.functions.size(); ++i) {
        FunctionInfo& fn = mod.functions[i];
        std::vector<const ast::Stmt*> body;
        body.reserve(fn.decl->body.size());
        for (const auto& s : fn.decl->body) body.push_back(s.get());

        FunctionAnalysisArgs args;
        args.parent = nullptr;
        args.qualifiedName = fn.name;
        args.moduleIndex = i;
        args.site = nullptr;
        args.directCallable = fn.directCallable;
        args.params = &fn.decl->params;
        args.paramTypes = fn.signature.params;
        args.body = std::move(body);
        args.span = fn.decl->span;
        args.record = record;
        args.isGenerator = fn.decl->isGenerator || fn.decl->isAsync;

        const auto outcome = analyzeFunction(mod, args);
        if (!outcome.ok) return false;
        // Only a direct-callable function's return is a proof about its
        // callers; an escaping one is reached through the dynamic convention,
        // so its signature stays dynamic however clear its body is.
        if (fn.directCallable) fn.observedReturn = join(fn.observedReturn, outcome.returnType);
    }

    return runTopLevel(mod, split, record);
}

// Folds the pass's observations into the signatures. Returns whether anything
// moved; the join means a signature can only widen, so "nothing moved" is a
// real fixpoint and recursion terminates.
bool widenSignatures(ModuleContext& mod) {
    bool changed = false;
    for (auto& fn : mod.functions) {
        if (!fn.directCallable) continue;
        for (size_t p = 0; p < fn.signature.params.size(); ++p) {
            const Type widened = join(fn.signature.params[p], fn.observedParams[p]);
            if (widened != fn.signature.params[p]) {
                fn.signature.params[p] = widened;
                changed = true;
            }
        }
        const Type widenedReturn = join(fn.signature.returnType, fn.observedReturn);
        if (widenedReturn != fn.signature.returnType) {
            fn.signature.returnType = widenedReturn;
            changed = true;
        }
    }
    return changed;
}

// What a parameter is left with when pin optimism declined to join a Dynamic
// argument into it (flow_expr.cpp). The skip cannot be free: the join is what
// makes a parameter's type a FACT about every caller, and a parameter that also
// receives values this pass refused to look at was not watched being made. So
// the identity survives — a shape compare checks it, and a wrong guess costs a
// miss — and nothing else does. Spending it on a primitive is what turned the
// blanket probe's `mesh_churn_2k` run into a NaN: `builtHere` left true is a
// raw unbox of whatever the skipped call site passed.
//
// Monotone in the fixpoint's sense, which is why it belongs here and not at the
// contribution: `Never` stays `Never`, a narrowing identity widens to `Dynamic`
// along with the join it came from, and the mark itself only ever gets set.
Type demoteToGuess(Type t) {
    if (t.is(TypeKind::Never)) return Type::never();
    if (t.is(TypeKind::Object) && t.shapeClass() != kNoShapeClass) {
        return Type::objectIdentityOnly(t.shapeClass());
    }
    return Type::dynamic();
}

// The same fold for class methods, whose callers are enumerated through the
// receiver's class rather than through the callee's name (method_ident.h).
//
// A poisoned or non-plain method is widened to the uniform dynamic convention
// HERE rather than being skipped, so that the widening is part of the fixpoint:
// poison only grows, so a method that gives up its parameters mid-fixpoint has
// them dynamic on every later round, and the round that discovers it is the
// round that reports "changed".
bool widenMethods(ModuleContext& mod) {
    if (!mod.interprocIdent) return false;
    bool changed = false;
    for (uint32_t i = 0; i < mod.methods.methods().size(); ++i) {
        auto& m = mod.methods.methods()[i];
        const bool speaks = m.plainParams && !mod.methodPoison.poisons(i);
        for (size_t p = 0; p < m.signature.params.size(); ++p) {
            Type widened =
                speaks ? join(m.signature.params[p], m.observedParams[p]) : Type::dynamic();
            if (speaks && p < m.sawSkippedDynamicArg.size() && m.sawSkippedDynamicArg[p]) {
                widened = demoteToGuess(widened);
            }
            if (widened != m.signature.params[p]) {
                m.signature.params[p] = widened;
                changed = true;
            }
        }
        const Type widenedReturn =
            speaks ? join(m.signature.returnType, m.observedReturn) : Type::dynamic();
        if (widenedReturn != m.signature.returnType) {
            m.signature.returnType = widenedReturn;
            changed = true;
        }
    }
    return changed;
}

// The same fold for class CONSTRUCTORS, whose callers are enumerated by NAME:
// `new C(...)`, every `super(...)` that reaches C, and every default C's own
// parameter list writes (types/ctor_ident.h).
//
// A poisoned or non-plain constructor is widened to the uniform dynamic
// convention here rather than skipped, for the reason `widenMethods` gives: the
// widening has to be part of the fixpoint so that the round which discovers it
// is the round that reports "changed".
bool widenCtors(ModuleContext& mod) {
    if (!mod.ctorParamTypes) return false;
    bool changed = false;
    for (auto& c : mod.ctors.ctors()) {
        const bool speaks = c.plainParams && !mod.ctorPoison.poisons(c.className);
        for (size_t i = 0; i < c.signature.params.size(); ++i) {
            const Type widened =
                speaks ? join(c.signature.params[i], c.observedParams[i]) : Type::dynamic();
            if (widened != c.signature.params[i]) {
                c.signature.params[i] = widened;
                changed = true;
            }
        }
    }
    return changed;
}

bool finalizeUnreachedCtors(ModuleContext& mod) {
    if (!mod.ctorParamTypes) return false;
    bool changed = false;
    for (auto& c : mod.ctors.ctors()) {
        for (auto& param : c.signature.params) {
            if (!param.is(TypeKind::Never)) continue;
            param = Type::dynamic();
            c.unreached = true;
            changed = true;
        }
    }
    return changed;
}

// The class field-type harvest, re-run against the constructor signatures as
// they stand. This is the one road from a typed parameter to a typed FIELD:
// `this.x = x` says nothing until `x` does.
bool refineFieldHarvest(ModuleContext& mod, InferenceResult& result) {
    if (!mod.ctorParamTypes && !mod.methodParamTypes) return false;
    const auto ctorOracle = mod.ctorParamTypes ? mod.ctors.harvestOracle()
                                               : std::map<std::string, std::map<std::string, Type>>();
    if (mod.methodParamTypes) {
        const auto methodOracle = mod.methods.harvestOracle();
        return result.classLayouts.reharvestFieldTypes(ctorOracle, &methodOracle);
    }
    return result.classLayouts.reharvestFieldTypes(ctorOracle, nullptr);
}

// After the fixpoint settles: a parameter still at `Never` is one no call site
// this compilation saw ever reached. Widening it to `Dynamic` is what puts the
// method back on the uniform convention, and it cannot be done inside the loop —
// on the first round EVERY parameter is `Never`, and doing it there would hand
// the whole program the dynamic answer before a single call site had been read.
bool finalizeUnreachedMethods(ModuleContext& mod) {
    if (!mod.interprocIdent) return false;
    bool changed = false;
    for (auto& m : mod.methods.methods()) {
        for (auto& param : m.signature.params) {
            if (!param.is(TypeKind::Never)) continue;
            param = Type::dynamic();
            m.unreached = true;
            changed = true;
        }
        if (m.signature.returnType.is(TypeKind::Never)) {
            m.signature.returnType = Type::dynamic();
            changed = true;
        }
    }
    return changed;
}

void resetObservations(ModuleContext& mod) {
    for (auto& fn : mod.functions) {
        fn.observedParams.assign(fn.signature.params.size(), Type::never());
        fn.observedReturn = Type::never();
    }
    for (auto& m : mod.methods.methods()) {
        m.observedParams.assign(m.signature.params.size(), Type::never());
        m.observedReturn = Type::never();
    }
    for (auto& c : mod.ctors.ctors()) {
        c.observedParams.assign(c.signature.params.size(), Type::never());
    }
}

// Can any statement in the program change what `Math` means? The scan runs
// over the MERGED module — the whole program — so one pass answers for every
// call site. `Math.<name>` and `Math[expr]` READS are the one harmless
// mention; everything else taints: a bare `Math` (it aliases the object,
// and a later write through the alias is invisible here), a member write,
// update or delete through it, a destructuring that could target it, any
// mention of `globalThis` (the other road to the same object). A program
// binding NAMED Math needs no care here: writes through it touch the
// program's own object, and each claim site separately refuses shadowed
// names (`resolvesToUserBinding`) — this bit is about mutation of the
// builtin, which requires the builtin as a value, which requires a mention
// this scan sees.
//
// Sloppy-mode `this` is not a vector: bronze binds it undefined, never the
// global object (lowerThisValue), so `this.Math` reaches nothing.
class MathTaintScan final : public ast::detail::IdentVisitor {
public:
    bool tainted = false;

    void visit(const ast::Ident& i) override {
        if (i.name == "Math" || i.name == "globalThis") tainted = true;
    }
    void visit(const ast::MemberAccess& m) override {
        if (isMath(m.object.get())) return;
        ast::detail::IdentVisitor::visit(m);
    }
    void visit(const ast::IndexAccess& ix) override {
        if (isMath(ix.object.get())) {
            ix.index->accept(*this);
            return;
        }
        ast::detail::IdentVisitor::visit(ix);
    }
    void visit(const ast::Binary& b) override {
        if (ast::isAssignOp(b.op) && memberBaseIsMath(b.lhs.get())) tainted = true;
        ast::detail::IdentVisitor::visit(b);
    }
    void visit(const ast::Unary& u) override {
        const bool mutating = u.op == ast::UnaryOp::Delete || u.op == ast::UnaryOp::PreInc ||
                              u.op == ast::UnaryOp::PreDec || u.op == ast::UnaryOp::PostInc ||
                              u.op == ast::UnaryOp::PostDec;
        if (mutating && memberBaseIsMath(u.operand.get())) tainted = true;
        ast::detail::IdentVisitor::visit(u);
    }
    // A destructuring pattern's targets can be member expressions, and the
    // walk below cannot tell a target from a read — so any Math anywhere in
    // the pattern taints, via a plain mention scan over its expressions.
    void visit(const ast::DestructuringAssign& d) override {
        ast::detail::IdentVisitor mentions;
        ast::detail::visitPatternExprs(d.pattern.get(), mentions);
        if (mentions.names.count("Math") != 0 || mentions.names.count("globalThis") != 0) {
            tainted = true;
        }
        d.value->accept(*this);
    }

private:
    static bool isMath(const ast::Expr* e) {
        const auto* id = dynamic_cast<const ast::Ident*>(e);
        return id != nullptr && (id->name == "Math");
    }
    static bool memberBaseIsMath(const ast::Expr* e) {
        if (const auto* m = dynamic_cast<const ast::MemberAccess*>(e)) {
            return isMath(m->object.get());
        }
        if (const auto* ix = dynamic_cast<const ast::IndexAccess*>(e)) {
            return isMath(ix->object.get());
        }
        return false;
    }
};

}  // namespace

std::optional<InferenceResult> inferModule(const ast::Module& module, DiagnosticSink& diags,
                                           const std::vector<std::string>* hostGlobals,
                                           const PinManifest* pins) {
    InferenceResult result;
    result.moduleName = module.name;

    bool hostOverridesMath = false;
    std::vector<std::string> hostClaimNames;
    if (hostGlobals != nullptr) {
        for (const auto& g : *hostGlobals) {
            if (g == "Math" || g == "globalThis") hostOverridesMath = true;
            // A host-provided constructor is not the builtin the typed-array
            // proof names; feeding it to the module-scope set makes every
            // claim site see a user binding and stand down.
            if (g == "Math" || g == "Float64Array" || g == "Float32Array") {
                hostClaimNames.push_back(g);
            }
        }
    }

    const ModuleSplit split = splitModule(module);
    const std::set<std::string> escaping = escapingNames(module);

    ModuleContext mod;
    mod.result = &result;
    // Before any body is walked: the flow pass reads class identities to type
    // `this` and `new C()`, and it must read the SAME identity on the probe
    // passes and the recording pass, so the table cannot be built lazily as
    // `constructorShape` builds constructor-function shapes.
    result.classLayouts.build(module, result.shapes);
    // The write audit's syntactic half, for the same reason: it is read on
    // every round and a table that filled in as it went would answer
    // differently on the probe rounds and the recording round.
    mod.fieldAudit.scan(module);
    // Interprocedural identity, off behind its seam. Both halves are built
    // before any body is walked, for the same reason the class table is: the
    // flow pass reads them on every round, and a table that filled in as it went
    // would answer differently on the probe rounds and the recording round.
    mod.valueFlow = std::getenv("BRONZE_NO_VALUE_FLOW") == nullptr;
    mod.interprocIdent = std::getenv("BRONZE_NO_INTERPROC_IDENT") == nullptr;
    mod.methodParamTypes = std::getenv("BRONZE_NO_METHOD_PARAM_TYPES") == nullptr;
    // The two pin modes. `--pins` is the targeted one and needs no env var;
    // `BRONZE_UNSOUND_PINS` stays as the degenerate "pin everything" form the
    // ceiling probe was measured with, so that number remains reproducible.
    mod.unsoundPins = std::getenv("BRONZE_UNSOUND_PINS") != nullptr;
    mod.pins = pins != nullptr && !pins->empty() ? pins : nullptr;
    if (mod.interprocIdent) {
        mod.methods.build(module);
        scanMethodEscapes(module, mod.methods, mod.methodPoison);
    }
    mod.ctorParamTypes = std::getenv("BRONZE_NO_CTOR_PARAM_TYPES") == nullptr;
    if (mod.ctorParamTypes || mod.methodParamTypes) {
        if (mod.ctorParamTypes) {
            mod.ctors.build(module);
            scanCtorEscapes(module, mod.ctors, mod.ctorPoison, mod.ctorEscapes);
        }
        // The field harvest starts from the parameter signatures, which are all
        // `Never` here. That is the bottom of the same lattice everything else
        // in the loop below climbs, so the whole sequence is monotone from the
        // first round rather than from the second.
        refineFieldHarvest(mod, result);
    }
    mod.diags = &diags;
    for (const auto& name : ast::getScopeDeclarations(module.body)) {
        mod.moduleScopeNames.insert(name);
    }
    for (const auto& name : ast::getHoistedVarDeclarations(module.body)) {
        mod.moduleScopeNames.insert(name);
    }
    for (const auto& stmt : module.body) {
        if (const auto* imp = dynamic_cast<const ast::ImportDecl*>(stmt.get())) {
            for (const auto& spec : imp->specifiers) mod.moduleScopeNames.insert(spec.local);
        }
    }
    for (const auto& name : hostClaimNames) mod.moduleScopeNames.insert(name);
    if (!hostOverridesMath) {
        MathTaintScan taint;
        for (const auto& stmt : module.body) {
            if (stmt) stmt->accept(taint);
        }
        mod.mathPristine = !taint.tainted;
    }
    for (uint32_t i = 0; i < split.decls.size(); ++i) {
        const ast::FunctionDecl* decl = split.decls[i];
        FunctionInfo fn;
        fn.decl = decl;
        fn.name = decl->name;
        // The whole direct-callable test. `needsEnv` does not appear because it
        // cannot fire here: a module-level declaration is a module symbol, not
        // a closure, so lowering never gives it the synthetic `__env`
        // parameter. Only function *expressions* and nested declarations become
        // closures, and those have no module function index to be direct-called
        // through in the first place.
        fn.directCallable = escaping.count(decl->name) == 0;
        // A parameter with a default, a rest parameter, or a pattern breaks the
        // one-argument-per-parameter correspondence that a typed signature IS:
        // the value bound is not the value passed. Signatures are per source
        // parameter, so there is nothing to widen against.
        for (const auto& param : decl->params) {
            if (param.defaultValue || param.isRest || param.pattern) {
                fn.directCallable = false;
                break;
            }
        }
        fn.signature.params.assign(decl->params.size(),
                                   fn.directCallable ? Type::never() : Type::dynamic());
        fn.signature.returnType = fn.directCallable ? Type::never() : Type::dynamic();
        mod.functions.push_back(std::move(fn));
        // A duplicate top-level function name is lowering's error to report
        // (it owns the module symbol table); here the first one simply wins,
        // so inference never invents a second diagnostic for it.
        mod.indexByName.emplace(decl->name, i);
    }

    seedModuleBindings(mod, split);

    bool converged = false;
    bool finalized = false;
    for (uint32_t iter = 0; iter <= kMaxCallGraphIterations; ++iter) {
        resetObservations(mod);
        const size_t poisonBefore = mod.methodPoison.version();
        const size_t ctorPoisonBefore = mod.ctorPoison.version();
        // The module-binding table is joined into, never rebuilt, so a round
        // that widens one entry has to be a round that says "changed" — the
        // consumers of the table are the very bodies this pass just walked.
        const std::map<std::string, Type> bindingsBefore = mod.moduleBindings;
        if (!runPass(mod, split, /*record=*/false)) return std::nullopt;
        bool changed = widenSignatures(mod);
        changed = widenMethods(mod) || changed;
        changed = widenCtors(mod) || changed;
        // The harvest reads the signatures `widenCtors` just folded, and the
        // bodies that read the harvest are the ones the next round walks.
        changed = refineFieldHarvest(mod, result) || changed;
        changed = mod.methodPoison.version() != poisonBefore || changed;
        changed = mod.ctorPoison.version() != ctorPoisonBefore || changed;
        changed = mod.moduleBindings != bindingsBefore || changed;
        // The field audit folds in here, and its direction is the opposite of
        // every other fold in this loop: signatures only WIDEN and field
        // cleanliness only NARROWS. Both are monotone over a finite lattice, so
        // the combination still terminates — a refuted name widens the types
        // that read it, which can refute further names, which can never
        // un-refute one.
        changed = mod.fieldAudit.settle() || changed;
        if (changed) continue;
        // Nothing moved. The one widening that could not run inside the loop
        // does so now, and the loop re-enters to let its consequences settle:
        // a method put back on the dynamic convention makes its body's receivers
        // dynamic, which can poison further names.
        if (!finalized) {
            finalized = true;
            bool moved = finalizeUnreachedMethods(mod);
            moved = finalizeUnreachedCtors(mod) || moved;
            moved = refineFieldHarvest(mod, result) || moved;
            if (moved) continue;
        }
        converged = true;
        break;
    }
    if (!converged) {
        diags.error(Span{}, "internal: type inference call-graph signatures did not converge");
        return std::nullopt;
    }

    // One more walk, this time filling the side table. Everything it reads is
    // already at the fixpoint, so this pass cannot change any signature.
    resetObservations(mod);
    if (!runPass(mod, split, /*record=*/true)) return std::nullopt;

    if (mod.ctorParamTypes) {
        auto& rep = result.ctorParams;
        rep.valueEscapes = mod.ctorEscapes.valueEscapes;
        rep.valueEscapeReason = mod.ctorEscapes.valueEscapeReason;
        rep.globalPoison = mod.ctorPoison.all ? mod.ctorPoison.allReason : std::string();
        for (const auto& c : mod.ctors.ctors()) {
            if (!c.isForwarder) ++rep.ctors;
        }
        rep.unnamedNewSubtree = mod.unnamedNewSubtree;
        rep.unnamedNewIgnored = mod.unnamedNewIgnored;
        rep.unnamedNewAll = mod.unnamedNewAll;
        rep.classes = static_cast<uint32_t>(result.classLayouts.all().size());
        for (const auto& c : mod.ctors.ctors()) {
            // An implicit forwarder has no parameters of its own; counting it
            // would report the `...args` the parser wrote as a constructor whose
            // parameters the analysis failed to type.
            if (c.isForwarder) {
                ++rep.forwarders;
                continue;
            }
            rep.params += static_cast<uint32_t>(c.signature.params.size());
            if (!c.plainParams) {
                ++rep.ctorsNotPlain;
            } else if (mod.ctorPoison.poisons(c.className)) {
                ++rep.poisons[mod.ctorPoison.reasonFor(c.className)];
            } else if (c.unreached) {
                ++rep.ctorsUnreached;
            } else {
                ++rep.ctorsSpeaking;
            }
            for (const Type& param : c.signature.params) {
                if (param.is(TypeKind::Number)) {
                    ++rep.paramsNumber;
                } else if (param.is(TypeKind::Object)) {
                    ++rep.paramsObject;
                } else if (param.is(TypeKind::Dynamic)) {
                    ++rep.paramsDynamic;
                } else {
                    ++rep.paramsOther;
                }
            }
        }
    }

    if (mod.interprocIdent) {
        auto& rep = result.methodParams;
        rep.classes = static_cast<uint32_t>(result.classLayouts.all().size());
        rep.methods = static_cast<uint32_t>(mod.methods.methods().size());
        rep.globalPoison = mod.methodPoison.all ? mod.methodPoison.allReason : std::string();
        rep.unboundedCalls = mod.unboundedMethodCalls;
        for (uint32_t i = 0; i < mod.methods.methods().size(); ++i) {
            const auto& m = mod.methods.methods()[i];
            rep.params += static_cast<uint32_t>(m.signature.params.size());
            if (!m.plainParams) {
                ++rep.methodsNotPlain;
            } else if (mod.methodPoison.poisons(i)) {
                ++rep.poisons[mod.methodPoison.reasonFor(i)];
            } else if (m.unreached) {
                ++rep.methodsUnreached;
            } else {
                ++rep.methodsSpeaking;
            }
            for (const Type& param : m.signature.params) {
                if (param.is(TypeKind::Number)) {
                    ++rep.paramsNumber;
                } else if (param.is(TypeKind::Object)) {
                    ++rep.paramsObject;
                } else if (param.is(TypeKind::Dynamic)) {
                    ++rep.paramsDynamic;
                } else {
                    ++rep.paramsOther;
                }
            }
        }
    }

    result.fieldAudit.namesWritten = mod.fieldAudit.nameCount();
    result.fieldAudit.namesClean = mod.fieldAudit.cleanCount();
    result.fieldAudit.namesLocallyClean = mod.fieldAudit.locallyCleanCount();
    result.fieldAudit.globalRefusals = mod.fieldAudit.globalRefusals();
    result.fieldAudit.computedSites = mod.fieldAudit.computedSiteCount();
    result.fieldAudit.computedRefuted = mod.fieldAudit.computedRefutedCount();
    result.fieldAudit.computedKeyTypes = mod.fieldAudit.computedKeyTypes();
    result.fieldAudit.computedReceiverTypes = mod.fieldAudit.computedReceiverTypes();
    for (const auto& [name, why] : mod.fieldAudit.report()) {
        if (!why.empty()) {
            ++result.fieldAudit.refusals[why];
        } else if (mod.fieldAudit.numberClean(name)) {
            result.fieldAudit.cleanNames.push_back(name);
        }
    }
    for (const auto& r : mod.fieldAudit.residue()) {
        result.fieldAudit.residue.push_back(
            InferenceResult::FieldAuditReport::ResidueSite{r.reason, r.count, r.representativeSite});
    }

    result.moduleSignatures.reserve(mod.functions.size());
    result.moduleDirectCallable.reserve(mod.functions.size());
    result.moduleFunctionSlot.assign(mod.functions.size(), kNoFunctionIndex);
    for (const auto& fn : mod.functions) {
        result.moduleSignatures.push_back(fn.signature);
        result.moduleDirectCallable.push_back(fn.directCallable);
    }
    result.moduleFunctionIndex = mod.indexByName;

    for (uint32_t slot = 0; slot < result.functions.size(); ++slot) {
        const uint32_t index = result.functions[slot].index;
        if (index == kNoFunctionIndex) continue;
        result.moduleFunctionSlot[index] = slot;
        // The dumped signature is the calling convention, not the body's
        // opinion of itself: that is what a call site has to honour.
        result.functions[slot].signature = result.moduleSignatures[index];
    }
    return result;
}

}  // namespace bronze::types
