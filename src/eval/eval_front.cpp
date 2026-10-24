// The evaluator's front end (eval_internal.h says why it is its own file).

#include "eval/eval_internal.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "ast/query_walk.h"
#include "embed/embed.h"
#include "lower/lower.h"
#include "runtime/host_globals.h"
#include "runtime/module_registry.h"
#include "runtime/rt_builtins.h"
#include "support/timings.h"
#include "types/infer.h"

namespace bronze::eval::detail {

namespace {

class DirectCalleeVisitor final : public ast::detail::IdentVisitor {
public:
    std::unordered_set<std::string> called;
    void visit(const ast::Call& c) override {
        if (const auto* id = dynamic_cast<const ast::Ident*>(c.callee.get())) {
            called.insert(id->name);
        }
        ast::detail::IdentVisitor::visit(c);
    }
};

std::vector<ast::FunctionDecl*> prepareEvalAstForInference(ast::Module& astModule,
                                                           const std::string& resName) {
    if (!resName.empty() && !astModule.body.empty()) {
        if (auto* exprStmt = dynamic_cast<ast::ExprStmt*>(astModule.body.back().get())) {
            auto varDecl = std::make_unique<ast::VarDecl>();
            varDecl->name = resName;
            varDecl->isVar = true;
            varDecl->init = std::move(exprStmt->expr);
            astModule.body.back() = std::move(varDecl);
        }
    }

    DirectCalleeVisitor callees;
    for (const auto& stmt : astModule.body) {
        if (stmt) stmt->accept(callees);
    }
    std::vector<ast::FunctionDecl*> temporarilyExported;
    for (const auto& stmt : astModule.body) {
        if (auto* fd = dynamic_cast<ast::FunctionDecl*>(stmt.get())) {
            if (!fd->isExported && !fd->name.empty() &&
                fd->name.find('.') == std::string::npos &&
                callees.called.count(fd->name) == 0) {
                fd->isExported = true;
                temporarilyExported.push_back(fd);
            }
        }
    }
    return temporarilyExported;
}

void exportEvalDeclarations(ast::Module& astModule,
                            const std::vector<ast::FunctionDecl*>& temporarilyExported) {
    for (auto* fd : temporarilyExported) {
        fd->isExported = false;
    }
    std::vector<ast::StmtPtr> exports;
    for (const auto& stmt : astModule.body) {
        if (auto* vd = dynamic_cast<ast::VarDecl*>(stmt.get())) {
            if (vd->isVar && !vd->name.empty() && vd->name.find('.') == std::string::npos) {
                auto globalThisIdent = std::make_unique<ast::Ident>();
                globalThisIdent->name = "globalThis";
                auto member = std::make_unique<ast::MemberAccess>();
                member->object = std::move(globalThisIdent);
                member->property = vd->name;
                auto valIdent = std::make_unique<ast::Ident>();
                valIdent->name = vd->name;
                auto assign = std::make_unique<ast::Binary>();
                assign->op = ast::BinaryOp::Assign;
                assign->lhs = std::move(member);
                assign->rhs = std::move(valIdent);
                auto exportStmt = std::make_unique<ast::ExprStmt>();
                exportStmt->expr = std::move(assign);
                exports.push_back(std::move(exportStmt));
            }
        } else if (auto* fd = dynamic_cast<ast::FunctionDecl*>(stmt.get())) {
            if (!fd->name.empty() && fd->name.find('.') == std::string::npos) {
                auto globalThisIdent = std::make_unique<ast::Ident>();
                globalThisIdent->name = "globalThis";
                auto member = std::make_unique<ast::MemberAccess>();
                member->object = std::move(globalThisIdent);
                member->property = fd->name;
                auto valIdent = std::make_unique<ast::Ident>();
                valIdent->name = fd->name;
                auto assign = std::make_unique<ast::Binary>();
                assign->op = ast::BinaryOp::Assign;
                assign->lhs = std::move(member);
                assign->rhs = std::move(valIdent);
                auto exportStmt = std::make_unique<ast::ExprStmt>();
                exportStmt->expr = std::move(assign);
                exports.push_back(std::move(exportStmt));
            }
        }
    }
    for (auto& exp : exports) {
        astModule.body.push_back(std::move(exp));
    }
}

std::string readPins(const std::string& path) {
    if (path.empty()) return {};
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The realm's module registry, read at COMPILE time (eval.h says when a host
// turns this on). The paths come from the realm rather than from the host,
// because the realm is what knows which instances exist — a host would have to
// keep a second list and the two would drift the first time a unit failed
// halfway.
void applyModuleRegistry(const EvalOptions& options, modules::ModuleOptions& modOpts) {
    if (!options.moduleRegistry) return;
    modOpts.publishModules = true;
    modOpts.publishEntry = options.publishEntry;  // a page's module FILE entry
    if (!options.externalModules.empty() || options.externalModulesCaptured) {
        modOpts.externalModules = options.externalModules;
    } else {
        modOpts.externalModules = runtime::rtModuleRegistryPaths();
    }
}

}  // namespace

bool evalTimingsEnabled() {
    static const bool on = [] {
        const char* v = std::getenv("BRONZE_TIMINGS");
        const bool enabled = v && v[0] == '1';
        if (enabled) support::setTimingsEnabled(true);
        return enabled;
    }();
    return on;
}

void reportProgress(const EvalOptions& options, const char* phase, double fraction) {
    if (options.onProgress) options.onProgress(CompileProgress{phase, fraction});
}

bool gatherInputs(const EvalOptions& options, CompileInputs& in, DiagnosticSink& diags) {
    in.hostGlobals = options.hostGlobals;
    if (in.hostGlobals.empty()) {
        for (const auto& entry : runtime::rtHostGlobalEntries()) {
            in.hostGlobals.push_back(entry.first);
        }
    }

    // The natives the host registered, as the manifest the lowerer reads —
    // the SAME text an ahead-of-time build reads from a file, parsed by the
    // same reader, so the JIT and AOT paths cannot disagree about a
    // registration. This thread's registry, unless the options carry one
    // captured on the thread that will run the program. A registry the reader
    // refuses is a host bug the registry should already have refused; it is
    // reported, not skipped.
    if (options.nativeManifestJson) {
        in.manifestJson = *options.nativeManifestJson;
        in.deferNativeBind = true;
    } else if (!embed::hostNativeNames().empty()) {
        in.manifestJson = embed::nativeManifestJson();
    }
    if (!in.manifestJson.empty()) {
        std::string err;
        in.nativeManifest = lower::NativeManifest::parse(in.manifestJson, "<registry>", err);
        if (!in.nativeManifest) {
            diags.error(Span{}, err);
            return false;
        }
        for (const auto& root : in.nativeManifest->namespaceRoots()) {
            in.hostGlobals.push_back(root);
        }
    }

    in.pinsText = readPins(options.pinsPath);
    if (!in.pinsText.empty()) {
        std::string err;
        in.pins.parse(in.pinsText, options.pinsPath, err, /*allowObserved=*/true);
    }
    in.tier = options.tier.value_or(defaultTier());
    return true;
}

modules::ModuleOptions moduleOptionsFor(const EvalOptions& options) {
    modules::ModuleOptions modOpts;
    modOpts.moduleRoots = options.moduleRoots;
    modOpts.entryResolvesAs = options.entryResolvesAs;
    applyModuleRegistry(options, modOpts);
    return modOpts;
}

cache::Digest programKey(std::string_view processMaterial, const EvalOptions& options,
                         const modules::ModuleOptions& modOpts, const CompileInputs& in,
                         std::string_view entryText) {
    cache::KeyBuilder k;
    k.field(processMaterial);
    k.field(options.filename);
    k.field(options.entryResolvesAs.generic_string());
    k.field(options.retainSource ? "src" : "nosrc");
    k.field(options.censusOutPath);
    k.field(modOpts.publishModules ? (modOpts.publishEntry ? "pub+entry" : "pub") : "nopub");
    k.field("|roots");
    for (const auto& root : modOpts.moduleRoots) {
        k.field(root.prefix);
        k.field(root.target.generic_string());
    }
    k.field("|externals");
    std::vector<std::string> externals = modOpts.externalModules;
    std::sort(externals.begin(), externals.end());
    for (const auto& ext : externals) k.field(ext);
    k.field("|globals");
    for (const auto& g : in.hostGlobals) k.field(g);
    k.field("|natives");
    k.field(in.manifestJson);
    k.field(in.pinsText);
    k.field(entryText);
    return k.finish();
}

std::unique_ptr<il::Module> lowerProgram(ast::Module& astModule, const EvalOptions& options,
                                         const CompileInputs& in, const std::string& resName,
                                         DiagnosticSink& diags, SourceSet& sources) {
    support::PhaseTimer timer(evalTimingsEnabled(), 2);
    auto temporarilyExported = prepareEvalAstForInference(astModule, resName);
    const std::vector<std::string>& hostGlobals = in.hostGlobals;

    const char* noInferEnv = std::getenv("BRONZE_NO_INFER");
    const bool noInfer = noInferEnv && noInferEnv[0] == '1';
    std::optional<types::InferenceResult> inferred;
    if (!noInfer) {
        inferred = types::inferModule(astModule, diags,
                                      hostGlobals.empty() ? nullptr : &hostGlobals,
                                      in.pins.empty() ? nullptr : &in.pins);
        if (diags.hasErrors() || !inferred) return nullptr;
    }
    exportEvalDeclarations(astModule, temporarilyExported);
    timer.mark("infer");
    reportProgress(options, "infer", 0.60);

    auto lowered = lower::lowerModule(astModule, diags,
                                      inferred ? &*inferred : nullptr,
                                      hostGlobals.empty() ? nullptr : &hostGlobals,
                                      &sources,
                                      /*stats=*/nullptr,
                                      /*assumeNoBigInt=*/false,
                                      in.pins.empty() ? nullptr : &in.pins,
                                      options.censusOutPath,
                                      in.nativeManifest ? &*in.nativeManifest : nullptr);
    if (diags.hasErrors() || !lowered) return nullptr;
    if (options.deterministicFp || isDeterministicFpMode()) {
        for (auto& fn : lowered->functions) {
            fn.setDeterministicFp(true);
        }
    }
    timer.mark("lower");
    reportProgress(options, "lower", 0.88);
    return std::make_unique<il::Module>(std::move(*lowered));
}

}  // namespace bronze::eval::detail
