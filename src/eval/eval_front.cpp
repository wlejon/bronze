// The evaluator's front end (eval_internal.h says why it is its own file).

#include "eval/eval_internal.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>

#include "ast/query_walk.h"
#include "embed/embed.h"
#include "lower/lower.h"
#include "selfhost/selfhost_library.h"
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

// The script's completion value (ECMA-262 16.1.6 ScriptEvaluation, and each
// statement's own Evaluation) is the value of the last statement that gave
// one. An expression statement gives its value; if, the loops, switch and
// try give their body's, or undefined when it gave none (UpdateEmpty); a
// block or a labelled statement gives what its statements gave; a
// declaration, an empty block and a finally clause give nothing.
bool isCompound(const ast::Stmt* s) {
    return dynamic_cast<const ast::IfStmt*>(s) || dynamic_cast<const ast::WhileStmt*>(s) ||
           dynamic_cast<const ast::DoWhileStmt*>(s) || dynamic_cast<const ast::ForStmt*>(s) ||
           dynamic_cast<const ast::ForInStmt*>(s) || dynamic_cast<const ast::ForOfStmt*>(s) ||
           dynamic_cast<const ast::SwitchStmt*>(s) || dynamic_cast<const ast::TryStmt*>(s);
}

bool givesValue(const ast::Stmt* s) {
    if (s == nullptr) return false;
    if (dynamic_cast<const ast::ExprStmt*>(s) || isCompound(s)) return true;
    if (const auto* b = dynamic_cast<const ast::BlockStmt*>(s)) {
        return std::any_of(b->stmts.begin(), b->stmts.end(), [](const ast::StmtPtr& p) { return givesValue(p.get()); });
    }
    if (const auto* l = dynamic_cast<const ast::LabeledStmt*>(s)) return givesValue(l->body.get());
    return false;
}

ast::StmtPtr assignCompletion(const std::string& resName, ast::ExprPtr value, const bronze::Span& span) {
    auto target = std::make_unique<ast::Ident>();
    target->name = resName;
    target->span = span;
    auto assign = std::make_unique<ast::Binary>();
    assign->op = ast::BinaryOp::Assign;
    assign->lhs = std::move(target);
    assign->rhs = std::move(value);
    assign->span = span;
    auto stmt = std::make_unique<ast::ExprStmt>();
    stmt->expr = std::move(assign);
    stmt->span = span;
    return stmt;
}

// `resName = undefined`, before a compound statement: what it gives when its
// body gives nothing.
ast::StmtPtr resetCompletion(const std::string& resName, const bronze::Span& span) {
    auto undef = std::make_unique<ast::UndefinedLit>();
    undef->span = span;
    return assignCompletion(resName, std::move(undef), span);
}

void recordCompletion(std::vector<ast::StmtPtr>& stmts, const std::string& resName);

// Every expression statement in `s` (not in a nested function) assigns its
// value to resName, and every compound statement first assigns undefined.
void recordCompletionIn(ast::Stmt* s, const std::string& resName) {
    if (auto* i = dynamic_cast<ast::IfStmt*>(s)) {
        recordCompletion(i->thenBody, resName);
        recordCompletion(i->elseBody, resName);
    } else if (auto* w = dynamic_cast<ast::WhileStmt*>(s)) {
        recordCompletion(w->body, resName);
    } else if (auto* d = dynamic_cast<ast::DoWhileStmt*>(s)) {
        recordCompletion(d->body, resName);
    } else if (auto* f = dynamic_cast<ast::ForStmt*>(s)) {
        recordCompletion(f->body, resName);
    } else if (auto* fi = dynamic_cast<ast::ForInStmt*>(s)) {
        recordCompletion(fi->body, resName);
    } else if (auto* fo = dynamic_cast<ast::ForOfStmt*>(s)) {
        recordCompletion(fo->body, resName);
    } else if (auto* sw = dynamic_cast<ast::SwitchStmt*>(s)) {
        for (ast::SwitchCase& c : sw->cases) recordCompletion(c.body, resName);
    } else if (auto* t = dynamic_cast<ast::TryStmt*>(s)) {
        recordCompletion(t->body, resName);
        recordCompletion(t->catchBody, resName);
    } else if (auto* b = dynamic_cast<ast::BlockStmt*>(s)) {
        recordCompletion(b->stmts, resName);
    }
}

void recordCompletion(std::vector<ast::StmtPtr>& stmts, const std::string& resName) {
    std::vector<ast::StmtPtr> out;
    out.reserve(stmts.size());
    for (ast::StmtPtr& s : stmts) {
        if (auto* e = dynamic_cast<ast::ExprStmt*>(s.get())) {
            out.push_back(assignCompletion(resName, std::move(e->expr), s->span));
            continue;
        }
        // A labelled statement's body is reached through it: a compound one
        // is reset before the label.
        ast::Stmt* inner = s.get();
        while (auto* l = dynamic_cast<ast::LabeledStmt*>(inner)) {
            if (auto* e = dynamic_cast<ast::ExprStmt*>(l->body.get())) {
                l->body = assignCompletion(resName, std::move(e->expr), l->body->span);
                inner = nullptr;
                break;
            }
            inner = l->body.get();
        }
        if (inner != nullptr) {
            if (isCompound(inner)) out.push_back(resetCompletion(resName, s->span));
            recordCompletionIn(inner, resName);
        }
        out.push_back(std::move(s));
    }
    stmts = std::move(out);
}

std::vector<ast::FunctionDecl*> prepareEvalAstForInference(ast::Module& astModule,
                                                           const std::string& resName) {
    if (!resName.empty() && !astModule.body.empty()) {
        // The last statement that can give a value is the one whose value
        // the script completes with.
        auto last = astModule.body.end();
        for (auto it = astModule.body.begin(); it != astModule.body.end(); ++it) {
            if (givesValue(it->get())) last = it;
        }
        if (last != astModule.body.end()) {
            if (auto* exprStmt = dynamic_cast<ast::ExprStmt*>(last->get())) {
                // The common case, and the cheap one: its value, kept.
                auto varDecl = std::make_unique<ast::VarDecl>();
                varDecl->name = resName;
                varDecl->isVar = true;
                varDecl->init = std::move(exprStmt->expr);
                varDecl->span = exprStmt->span;
                *last = std::move(varDecl);
            } else {
                // A compound statement or a block: the expression statements
                // inside it (only it: anything before gave a value it
                // replaces) record theirs as they run.
                const bronze::Span span = (*last)->span;
                std::vector<ast::StmtPtr> one;
                one.push_back(std::move(*last));
                recordCompletion(one, resName);
                auto pos = astModule.body.erase(last);
                astModule.body.insert(pos, std::make_move_iterator(one.begin()), std::make_move_iterator(one.end()));
                auto varDecl = std::make_unique<ast::VarDecl>();
                varDecl->name = resName;
                varDecl->isVar = true;
                varDecl->span = span;
                astModule.body.insert(astModule.body.begin(), std::move(varDecl));
            }
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

// The trailing expression's value always goes to the global object, under
// `resName`, where the host reads it; the unit's other top-level `var`s and
// function declarations only with EvalOptions::publishTopLevel.
void exportEvalDeclarations(ast::Module& astModule,
                            const std::vector<ast::FunctionDecl*>& temporarilyExported,
                            const std::string& resName, bool publishTopLevel) {
    for (auto* fd : temporarilyExported) {
        fd->isExported = false;
    }
    std::vector<ast::StmtPtr> exports;
    for (const auto& stmt : astModule.body) {
        if (auto* vd = dynamic_cast<ast::VarDecl*>(stmt.get())) {
            if (!publishTopLevel && (resName.empty() || vd->name != resName)) continue;
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
            if (publishTopLevel && !fd->name.empty() && fd->name.find('.') == std::string::npos) {
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

std::string readOptionalFile(const std::string& path) {
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

bool gatherInputs(const EvalOptions& options, CompileInputs& in, DiagnosticSink& diags, bool keyOnly) {
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

    in.feedbackText = readOptionalFile(options.feedbackInPath);
    if (!keyOnly && !in.feedbackText.empty()) in.feedback = spec::TypeProfile::parse(in.feedbackText, nullptr);
    return true;
}

modules::ModuleOptions moduleOptionsFor(const EvalOptions& options) {
    modules::ModuleOptions modOpts;
    modOpts.moduleRoots = options.moduleRoots;
    modOpts.entryResolvesAs = options.entryResolvesAs;
    if (options.classicScript) modOpts.classicScriptMarker = options.classicScriptMarker;
    applyModuleRegistry(options, modOpts);
    return modOpts;
}

cache::Digest programKey(std::string_view processMaterial, const EvalOptions& options,
                         const modules::ModuleOptions& modOpts, const CompileInputs& in,
                         std::string_view entryText, bool withFeedback, std::vector<cache::KeyPart>* parts) {
    cache::KeyBuilder k;
    k.part("environment");
    k.field(processMaterial);
    k.part("file name");
    k.field(options.filename);
    k.field(options.entryResolvesAs.generic_string());
    k.part("options");
    k.field(options.retainSource ? "src" : "nosrc");
    k.field(modOpts.publishModules ? (modOpts.publishEntry ? "pub+entry" : "pub") : "nopub");
    k.field(options.classicScript ? "classic:" + options.classicScriptMarker : "unit");
    k.field(options.publishTopLevel ? "publish-top" : "own-top");
    k.part("module roots");
    k.field("|roots");
    for (const auto& root : modOpts.moduleRoots) {
        k.field(root.prefix);
        k.field(root.target.generic_string());
    }
    k.part("module registry");
    k.field("|externals");
    std::vector<std::string> externals = modOpts.externalModules;
    std::sort(externals.begin(), externals.end());
    for (const auto& ext : externals) k.field(ext);
    k.part("host globals");
    k.field("|globals");
    for (const auto& g : in.hostGlobals) k.field(g);
    k.part("natives");
    k.field("|natives");
    k.field(in.manifestJson);
    k.part("profile");
    if (withFeedback) {
        k.field("|feedback");
        k.field(in.feedbackText);
    }
    k.part("text");
    k.field(entryText);
    if (parts) *parts = k.parts();
    return k.finish();
}

std::string moduleResName(const EvalOptions& options, const modules::ModuleOptions& modOpts,
                          const CompileInputs& in, std::string_view entryText) {
    const cache::Digest key =
        programKey(cache::moduleKeyMaterial(), options, modOpts, in, entryText, /*withFeedback=*/false);
    return "__bronze_eval_res_m" + key.hex().substr(0, 16);
}

std::unique_ptr<il::Module> lowerProgram(ast::Module& astModule, const EvalOptions& options,
                                         const CompileInputs& in, const std::string& resName,
                                         DiagnosticSink& diags, SourceSet& sources,
                                         bool aheadOfTime) {
    support::PhaseTimer timer(evalTimingsEnabled(), 2);
    if (options.classicScript) instantiateClassicScripts(astModule, options.classicScriptMarker);
    auto temporarilyExported = prepareEvalAstForInference(astModule, resName);
    const std::vector<std::string>& hostGlobals = in.hostGlobals;

    std::optional<types::InferenceResult> inferred =
        types::inferModule(astModule, diags, hostGlobals.empty() ? nullptr : &hostGlobals);
    if (diags.hasErrors() || !inferred) return nullptr;
    exportEvalDeclarations(astModule, temporarilyExported, resName, options.publishTopLevel);
    timer.mark("infer");
    reportProgress(options, "infer", 0.60);

    std::optional<il::Module> lowered;
    if (aheadOfTime) {
        // A module build: the ahead-of-time front end (lower.h), which a
        // `bronze build` goes through too. Its profile or harvest
        // (EvalOptions::feedbackInPath, `in.feedback`) is applied after
        // lowering, by the optimiser pipeline buildModule runs.
        lower::AheadOfTimeOptions aot;
        aot.hostGlobals = &hostGlobals;
        aot.nativeManifest = in.nativeManifest ? &*in.nativeManifest : nullptr;
        aot.selfHostLibrary = selfhost::library();
        lowered = lower::lowerAheadOfTime(astModule, diags, &*inferred, &sources, aot);
    } else {
        lowered = lower::lowerModule(astModule, diags, &*inferred,
                                     hostGlobals.empty() ? nullptr : &hostGlobals, &sources,
                                     in.nativeManifest ? &*in.nativeManifest : nullptr);
    }
    if (diags.hasErrors() || !lowered) return nullptr;
    timer.mark("lower");
    reportProgress(options, "lower", 0.88);
    return std::make_unique<il::Module>(std::move(*lowered));
}

}  // namespace bronze::eval::detail
