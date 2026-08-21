// For `getenv`, which MSVC deprecates and every other toolchain does not. The
// compile-time seam below is read exactly once, from a single-threaded driver —
// the thread-safety the _s variants buy has nothing to hold onto here. Same
// reasoning, and same one-line define, as lower_infer.cpp.
#define _CRT_SECURE_NO_WARNINGS

#include "types/infer.h"

#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "ast/queries.h"
#include "ast/query_walk.h"
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

// One walk of every function in the module, plus the top level. Call sites
// discovered here widen the callee's `observedParams`, which is what the
// outer fixpoint folds back into the signatures.
bool runPass(ModuleContext& mod, const ModuleSplit& split, bool record) {
    for (uint32_t i = 0; i < mod.functions.size(); ++i) {
        FunctionInfo& fn = mod.functions[i];
        std::vector<const ast::Stmt*> body;
        body.reserve(fn.decl->body.size());
        for (const auto& s : fn.decl->body) body.push_back(s.get());

        const auto outcome =
            analyzeFunction(mod, /*parent=*/nullptr, fn.name, i, /*site=*/nullptr,
                            fn.directCallable, fn.decl->params, fn.signature.params, body,
                            fn.decl->span, record,
                            fn.decl->isGenerator || fn.decl->isAsync);
        if (!outcome.ok) return false;
        // Only a direct-callable function's return is a proof about its
        // callers; an escaping one is reached through the dynamic convention,
        // so its signature stays dynamic however clear its body is.
        if (fn.directCallable) fn.observedReturn = join(fn.observedReturn, outcome.returnType);
    }

    if (!split.topLevel.empty()) {
        Span span{};
        span.begin = split.topLevel.front()->span.begin;
        span.end = split.topLevel.back()->span.end;
        const auto outcome = analyzeFunction(mod, /*parent=*/nullptr, kTopLevelName,
                                             kNoFunctionIndex, /*site=*/nullptr,
                                             /*directCallable=*/false, {}, {},
                                             split.topLevel, span, record);
        if (!outcome.ok) return false;
    }
    return true;
}

// Folds the pass's observations into the signatures. Returns whether anything
// moved; the join means a signature can only widen, so "nothing moved" is a
// real fixpoint and recursion terminates.
bool widenSignatures(ModuleContext& mod) {
    bool changed = false;
    for (auto& fn : mod.functions) {
        if (!fn.directCallable) continue;
        for (size_t i = 0; i < fn.signature.params.size(); ++i) {
            const Type widened = join(fn.signature.params[i], fn.observedParams[i]);
            if (widened != fn.signature.params[i]) {
                fn.signature.params[i] = widened;
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
    for (auto& m : mod.methods.methods()) {
        const bool speaks = m.plainParams && !mod.methodPoison.poisons(m.methodName);
        for (size_t i = 0; i < m.signature.params.size(); ++i) {
            const Type widened =
                speaks ? join(m.signature.params[i], m.observedParams[i]) : Type::dynamic();
            if (widened != m.signature.params[i]) {
                m.signature.params[i] = widened;
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
                                           const std::vector<std::string>* hostGlobals) {
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
    // Interprocedural identity, off behind its seam. Both halves are built
    // before any body is walked, for the same reason the class table is: the
    // flow pass reads them on every round, and a table that filled in as it went
    // would answer differently on the probe rounds and the recording round.
    mod.interprocIdent = std::getenv("BRONZE_NO_INTERPROC_IDENT") == nullptr;
    if (mod.interprocIdent) {
        mod.methods.build(module);
        scanMethodEscapes(module, mod.methods, mod.methodPoison);
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

    bool converged = false;
    bool finalized = false;
    for (uint32_t iter = 0; iter <= kMaxCallGraphIterations; ++iter) {
        resetObservations(mod);
        const size_t poisonBefore = mod.methodPoison.version();
        if (!runPass(mod, split, /*record=*/false)) return std::nullopt;
        bool changed = widenSignatures(mod);
        changed = widenMethods(mod) || changed;
        changed = mod.methodPoison.version() != poisonBefore || changed;
        if (changed) continue;
        // Nothing moved. The one widening that could not run inside the loop
        // does so now, and the loop re-enters to let its consequences settle:
        // a method put back on the dynamic convention makes its body's receivers
        // dynamic, which can poison further names.
        if (!finalized) {
            finalized = true;
            if (finalizeUnreachedMethods(mod)) continue;
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
