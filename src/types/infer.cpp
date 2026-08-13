#include "types/infer.h"

#include <set>
#include <string>
#include <vector>

#include "types/escape.h"
#include "types/flow.h"

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

void resetObservations(ModuleContext& mod) {
    for (auto& fn : mod.functions) {
        fn.observedParams.assign(fn.signature.params.size(), Type::never());
        fn.observedReturn = Type::never();
    }
}

}  // namespace

std::optional<InferenceResult> inferModule(const ast::Module& module, DiagnosticSink& diags) {
    InferenceResult result;
    result.moduleName = module.name;

    const ModuleSplit split = splitModule(module);
    const std::set<std::string> escaping = escapingNames(module);

    ModuleContext mod;
    mod.result = &result;
    mod.diags = &diags;
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
    for (uint32_t iter = 0; iter <= kMaxCallGraphIterations; ++iter) {
        resetObservations(mod);
        if (!runPass(mod, split, /*record=*/false)) return std::nullopt;
        if (!widenSignatures(mod)) {
            converged = true;
            break;
        }
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
