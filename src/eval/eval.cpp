#include "eval/eval.h"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <brass/runtime/compile_pool.hpp>

#include "ast/ast.h"
#include "il/print.h"
#include "codegen-brass/brass_backend.h"
#include "codegen-brass/brass_jit.h"
#include "embed/embed.h"
#include "lex/lexer.h"
#include "modules/modules.h"
#include "lower/lower.h"
#include "lower/native_manifest.h"
#include "parse/parser.h"
#include "runtime/exception.h"
#include "runtime/gc.h"
#include "runtime/host_globals.h"
#include "runtime/module_instance.h"
#include "runtime/module_registry.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/value.h"
#include "support/diagnostics.h"
#include "support/source.h"
#include "types/infer.h"
#include "types/pins.h"

namespace bronze::eval {

namespace {

static std::vector<std::shared_ptr<BrassTieredProgram>>& retainedPrograms() {
    static auto* list = new std::vector<std::shared_ptr<BrassTieredProgram>>();
    return *list;
}
static std::unordered_set<const BrassTieredProgram*>& retainedSet() {
    static auto* set = new std::unordered_set<const BrassTieredProgram*>();
    return *set;
}
static std::mutex g_programsMutex;
static std::mutex g_jitCompileMutex;

// The programs compiled with EvalOptions::shareAcrossThreads, by everything
// their compile read (sharedProgramKey). Guarded by g_jitCompileMutex.
struct SharedProgram {
    std::shared_ptr<BrassTieredProgram> program;
    std::string resName;
};
static std::unordered_map<std::string, SharedProgram>& sharedPrograms() {
    static auto* map = new std::unordered_map<std::string, SharedProgram>();
    return *map;
}
static std::atomic<uint64_t> s_evalCounter{0};
static std::atomic<ExecutionTier> s_defaultTier{ExecutionTier::Auto};

// At exit, no retained program may still be compiling in the background:
// a compile worker would be running brass while the process tears brass's
// statics down. Queued compiles are dropped, the ones in flight finish, and
// the process's compile pool stops.
void stopRetainedBackgroundCompiles() {
    if (brass::runtime::process_exiting()) return;
    stopBackgroundCompiles();
    brass::runtime::CompilePool::shared().shutdown();
}

void transformEvalAst(ast::Module& astModule, const std::string& resName) {
    if (!resName.empty() && !astModule.body.empty()) {
        if (auto* exprStmt = dynamic_cast<ast::ExprStmt*>(astModule.body.back().get())) {
            auto varDecl = std::make_unique<ast::VarDecl>();
            varDecl->name = resName;
            varDecl->isVar = true;
            varDecl->init = std::move(exprStmt->expr);
            astModule.body.back() = std::move(varDecl);
        }
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

modules::ModuleOptions moduleOptionsFor(const EvalOptions& options);

// Everything a compile reads: two compiles with the same key produce the
// same program. Each field is length-prefixed, so no two keys run together.
std::string sharedProgramKey(const EvalOptions& options, ExecutionTier tier,
                             const std::vector<std::string>& hostGlobals, const std::string& manifestJson,
                             const std::string& pinsText, const SourceSet& sources) {
    std::string key;
    auto field = [&](std::string_view s) {
        key += std::to_string(s.size());
        key += ':';
        key.append(s.data(), s.size());
    };
    field(executionTierToString(tier));
    field(options.filename);
    field(options.entryResolvesAs.string());
    field(options.retainSource ? "src" : "nosrc");
    field(options.emitDebugInfo ? "dbg" : "nodbg");
    field(options.censusOutPath);
    const modules::ModuleOptions modOpts = moduleOptionsFor(options);
    field(modOpts.publishModules ? (modOpts.publishEntry ? "pub+entry" : "pub") : "nopub");
    for (const auto& ext : modOpts.externalModules) field(ext);
    field("|globals");
    for (const auto& g : hostGlobals) field(g);
    field("|natives");
    field(manifestJson);
    field(pinsText);
    for (size_t i = 0; i < sources.size(); ++i) {
        const SourceBuffer& buf = sources.at(static_cast<uint16_t>(i));
        field(buf.name());
        field(buf.text());
    }
    return key;
}

// The one compile path: the program lowered to IL, then handed to the
// tiered engine at the options' tier (the process default when unset). With
// shareAcrossThreads, a program compiled before from the same inputs is
// reused (EvalOptions says when), and `resName` becomes its result name.
std::shared_ptr<BrassTieredProgram> compileAst(
    std::unique_ptr<ast::Module> astModule,
    const EvalOptions& options,
    std::string& resName,
    DiagnosticSink& diags,
    SourceSet& sources) {

    if (!astModule) return nullptr;
    std::lock_guard<std::mutex> compileLock(g_jitCompileMutex);

    std::vector<std::string> hostGlobals = options.hostGlobals;
    if (hostGlobals.empty()) {
        for (const auto& entry : runtime::rtHostGlobalEntries()) {
            hostGlobals.push_back(entry.first);
        }
    }

    // The natives this thread's host registered, as the manifest the lowerer
    // reads — the SAME text an ahead-of-time build reads from a file, parsed
    // by the same reader, so the JIT and AOT paths cannot disagree about a
    // registration. A registry the reader refuses is a host bug the registry
    // should already have refused; it is reported, not skipped.
    std::optional<lower::NativeManifest> nativeManifest;
    if (!embed::hostNativeNames().empty()) {
        std::string err;
        nativeManifest = lower::NativeManifest::parse(embed::nativeManifestJson(), "<registry>", err);
        if (!nativeManifest) {
            diags.error(Span{}, err);
            return nullptr;
        }
        for (const auto& root : nativeManifest->namespaceRoots()) {
            hostGlobals.push_back(root);
        }
    }

    const std::string pinsText = readPins(options.pinsPath);
    types::PinManifest pins;
    if (!pinsText.empty()) {
        std::string err;
        pins.parse(pinsText, options.pinsPath, err, /*allowObserved=*/true);
    }

    const ExecutionTier tier = options.tier.value_or(defaultTier());
    const bool share = options.shareAcrossThreads && tier != ExecutionTier::Tier2_Optimized;
    std::string shareKey;
    if (share) {
        shareKey = sharedProgramKey(options, tier, hostGlobals,
                                    nativeManifest ? embed::nativeManifestJson() : std::string(), pinsText, sources);
        auto it = sharedPrograms().find(shareKey);
        if (it != sharedPrograms().end() && !runtime::rtThreadHasModuleInstance(it->second.program->moduleSlotCell())) {
            resName = it->second.resName;
            return it->second.program;
        }
    }
    transformEvalAst(*astModule, resName);

    auto inferred = types::inferModule(*astModule, diags,
                                       hostGlobals.empty() ? nullptr : &hostGlobals,
                                       pins.empty() ? nullptr : &pins);
    if (diags.hasErrors() || !inferred) return nullptr;

    auto ilModule = lower::lowerModule(*astModule, diags,
                                       inferred ? &*inferred : nullptr,
                                       hostGlobals.empty() ? nullptr : &hostGlobals,
                                       &sources,
                                       /*stats=*/nullptr,
                                       /*assumeNoBigInt=*/false,
                                       pins.empty() ? nullptr : &pins,
                                       options.censusOutPath,
                                       nativeManifest ? &*nativeManifest : nullptr);
    if (diags.hasErrors() || !ilModule) return nullptr;

    if (!options.retainSource) {
        ilModule->sourceTexts.clear();
    }

    const uint64_t evalId = s_evalCounter.fetch_add(1, std::memory_order_relaxed);
    const std::string entrySym = "__bronze_dyn_entry_" + std::to_string(evalId);

    TieredEngineConfig config;
    config.tier = tier;
    config.entrySymbol = entrySym;
    config.hostGlobals = hostGlobals;
    config.emitDebugInfo = options.emitDebugInfo;

    std::shared_ptr<BrassTieredProgram> program = BrassTieredEngine(config).compile(*ilModule, diags);
    if (!program) return nullptr;
    // Bind the program's import table from the registry it was compiled
    // against, before anything runs. The entry rebinds on its own first
    // instruction and would be FATAL on a gap; doing it here first turns a
    // native unregistered between compile and run into a diagnostic naming
    // it instead.
    if (!ilModule->nativeImports.empty()) {
        void* table = program->symbolAddress(entrySym + "_native_imports");
        std::vector<std::string> missing;
        if (!table || !embed::bindNativeImports(table, &missing)) {
            std::string msg = "native imports unbound: the host registered no native for";
            for (const auto& m : missing) msg += "\n  " + m;
            if (!table) msg += "\n  (the program's import table symbol is missing)";
            diags.error(Span{}, msg);
            return nullptr;
        }
    }
    if (share) sharedPrograms()[shareKey] = SharedProgram{program, resName};
    return program;
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
    if (!options.externalModules.empty()) {
        modOpts.externalModules = options.externalModules;
    } else {
        modOpts.externalModules = runtime::rtModuleRegistryPaths();
    }
}

modules::ModuleOptions moduleOptionsFor(const EvalOptions& options) {
    modules::ModuleOptions modOpts;
    modOpts.moduleRoots = options.moduleRoots;
    modOpts.entryResolvesAs = options.entryResolvesAs;
    applyModuleRegistry(options, modOpts);
    return modOpts;
}

embed::CallResult runProgramAndCollectResult(
    std::shared_ptr<BrassTieredProgram> program,
    const std::string& resName,
    embed::ModuleHandle* moduleHandleOut) {

    auto* programPtr = program.get();
    retainProgram(std::move(program));

    // The bracket a host asked for: opened immediately before the entry, so
    // the spans the entry registers carry the handle, and closed after the
    // microtask checkpoint, so a later program's registrations do not. The
    // handle is written even when the entry throws — a top level that got
    // halfway registered its spans on the way, and they are still the host's
    // to unload.
    const embed::ModuleHandle handle = moduleHandleOut ? embed::beginModuleLoad() : 0;
    if (moduleHandleOut) *moduleHandleOut = handle;

    // The entry's value — a module with top-level await returns its promise —
    // is held across the drain, which runs the whole rest of such a module
    // and so collects: a raw copy of the bits would name the promise's
    // pre-collection address by the time isPromise reads its shape.
    // A top-level throw is caught here and handed back after the checkpoint:
    // jobs the top level queued before it threw still run.
    Value entryOut = Value::fromUndefined();
    bool threw = false;
    {
        bronze::ShadowStackFrame stackFrame;
        threw = runtime::rtTryCatch(
            [&] { entryOut = Value(programPtr->run().as_u64()); }, entryOut);
    }
    embed::Persistent entryVal{entryOut};
    embed::drainMicrotasks();
    embed::endModuleLoad(handle);

    if (threw) return embed::CallResult{entryVal.get(), /*thrown=*/true};

    if (entryVal.get().isObject() && embed::isPromise(entryVal.get())) {
        return embed::CallResult{entryVal.get(), /*thrown=*/false};
    }

    embed::GlobalValue g = embed::globalValue(resName);
    embed::Persistent result{g.found ? g.value : embed::undefined()};

    embed::GlobalValue glob = embed::globalValue("globalThis");
    if (glob.found) {
        embed::deleteProperty(glob.value, resName);
    }

    return embed::CallResult{result.get(), /*thrown=*/false};
}

std::unique_ptr<CompiledScript> newScript(const EvalOptions& options) {
    auto res = std::make_unique<CompiledScript>();
    const uint64_t evalId = s_evalCounter.fetch_add(1, std::memory_order_relaxed);
    res->resName = "__bronze_eval_res_" + std::to_string(evalId);
    res->tier = options.tier.value_or(defaultTier());
    return res;
}

void finishScript(CompiledScript& res, DiagnosticSink& diags, const SourceSet& sources) {
    res.success = res.program != nullptr;
    if (!res.success) res.errorMessage = diags.render(sources);
}

// `new Function(...)`'s source, compiled as a script that stores the function
// on the global object under a fresh name; returns it and removes the name.
Value evalFunctionSource(const std::string& prefix, const std::string& params, std::string_view body) {
    const uint64_t fnId = s_evalCounter.fetch_add(1, std::memory_order_relaxed);
    const std::string fnName = "__bronze_dyn_fn_" + std::to_string(fnId);
    const std::string fnCode = "globalThis." + fnName + " = " + prefix + params + "\n) {\n" + std::string(body) + "\n};";

    embed::CallResult cr = evalScript(fnCode, EvalOptions{.filename = "<Function>"});
    if (cr.thrown) runtime::rtThrow(cr.value);

    embed::GlobalValue g = embed::globalValue(fnName);
    if (!g.found) {
        return runtime::rtThrowSyntaxError("Function: created function was not found");
    }

    embed::Persistent fnObj{g.value};

    embed::GlobalValue glob = embed::globalValue("globalThis");
    if (glob.found) {
        embed::deleteProperty(glob.value, fnName);
    }

    return fnObj.get();
}

const char* functionPrefix(runtime::DynamicFunctionKind kind) {
    switch (kind) {
        case runtime::DynamicFunctionKind::Generator: return "function* anonymous(";
        case runtime::DynamicFunctionKind::Async: return "async function anonymous(";
        case runtime::DynamicFunctionKind::AsyncGenerator: return "async function* anonymous(";
        case runtime::DynamicFunctionKind::Ordinary:
        default: return "function anonymous(";
    }
}

}  // namespace

void setDefaultTier(ExecutionTier tier) {
    s_defaultTier.store(tier, std::memory_order_relaxed);
}

ExecutionTier defaultTier() {
    return s_defaultTier.load(std::memory_order_relaxed);
}

void retainProgram(std::shared_ptr<BrassTieredProgram> program) {
    if (!program) return;
    static std::once_flag atExitOnce;
    std::call_once(atExitOnce, [] { std::atexit(&stopRetainedBackgroundCompiles); });
    std::lock_guard<std::mutex> lock(g_programsMutex);
    // A program shared across threads is run, and retained, by each.
    if (!retainedSet().insert(program.get()).second) return;
    retainedPrograms().push_back(std::move(program));
}

void stopBackgroundCompiles() {
    std::lock_guard<std::mutex> lock(g_programsMutex);
    for (auto& program : retainedPrograms()) program->stopBackgroundCompilation();
}

void clearRetainedPrograms() {
    {
        std::lock_guard<std::mutex> compileLock(g_jitCompileMutex);
        sharedPrograms().clear();
    }
    std::lock_guard<std::mutex> lock(g_programsMutex);
    retainedSet().clear();
    retainedPrograms().clear();
}

std::unique_ptr<CompiledScript> compileScript(std::string_view source, const EvalOptions& options) {
    if (source.empty()) {
        auto res = std::make_unique<CompiledScript>();
        res->success = true;
        return res;
    }
    auto res = newScript(options);
    SourceSet sources;
    DiagnosticSink diags;
    auto astModule = modules::loadProgramSource(std::string(source), options.filename, sources, diags,
                                                moduleOptionsFor(options));
    if (!diags.hasErrors() && astModule) {
        res->program = compileAst(std::move(astModule), options, res->resName, diags, sources);
    }
    finishScript(*res, diags, sources);
    return res;
}

std::unique_ptr<CompiledScript> compileFile(const std::string& filePath, const EvalOptions& options) {
    auto res = newScript(options);
    SourceSet sources;
    DiagnosticSink diags;
    auto astModule = modules::loadProgram(filePath, sources, diags, moduleOptionsFor(options));
    if (!diags.hasErrors() && astModule) {
        res->program = compileAst(std::move(astModule), options, res->resName, diags, sources);
    }
    finishScript(*res, diags, sources);
    return res;
}

embed::CallResult runCompiledScript(std::unique_ptr<CompiledScript> script, const EvalOptions& options) {
    bronze::ShadowStackFrame rootFrame;
    if (!script) {
        return embed::CallResult{embed::undefined(), false};
    }

    if (!script->success) {
        std::string err = !script->errorMessage.empty() ? script->errorMessage : "compilation failed";
        Value syntaxErr;
        runtime::rtTryCatch([&] { runtime::rtThrowSyntaxError(err); }, syntaxErr);
        return embed::CallResult{syntaxErr, /*thrown=*/true};
    }

    if (!script->program) {
        return embed::CallResult{embed::undefined(), false};
    }
    return runProgramAndCollectResult(std::move(script->program), script->resName, options.moduleHandleOut);
}

embed::CallResult evalScript(std::string_view source, const EvalOptions& options) {
    auto script = compileScript(source, options);
    return runCompiledScript(std::move(script), options);
}

embed::CallResult evalFile(const std::string& filePath, const EvalOptions& options) {
    auto script = compileFile(filePath, options);
    return runCompiledScript(std::move(script), options);
}

Value evalScriptDirect(std::string_view source, const EvalOptions& options) {
    embed::CallResult cr = evalScript(source, options);
    if (cr.thrown) runtime::rtThrow(cr.value);
    return cr.value;
}

Value evalFunction(runtime::DynamicFunctionKind kind, std::span<const Value> args) {
    std::string params;
    std::string body;
    for (size_t i = 0; i < args.size(); ++i) {
        const bool last = (i + 1 == args.size());
        std::string text;
        if (args[i].isUndefined()) {
            text = "undefined";
        } else {
            text = embed::toUtf8(args[i]);
        }
        if (last) {
            body = std::move(text);
        } else {
            if (!params.empty()) params += ", ";
            params += text;
        }
    }
    return evalFunctionSource(functionPrefix(kind), params, body);
}

Value evalFunction(std::span<const std::string> params, std::string_view body,
                   runtime::DynamicFunctionKind kind) {
    std::string paramList;
    for (size_t i = 0; i < params.size(); ++i) {
        if (i > 0) paramList += ", ";
        paramList += params[i];
    }
    return evalFunctionSource(functionPrefix(kind), paramList, body);
}

// Installed by whoever means to run dynamic code — `bronze run` / `bronze -e`
// (cli/run.cpp) and the eval tests — and by NOTHING else. The shared runtime
// carries the eval library, and a `bronze build` executable is the host plus
// that runtime: its `Function("...")` must stay the refusal the AOT contract
// promises (builtin_function.cpp), not a JIT that happens to be in the same
// DLL. A host that wants dynamic code asks for it, as bro does through
// embed::setDynamicFunctionHook.
void installDefaultDynamicHooks() {
    auto evalHook = [](Value source) -> Value {
        if (!source.isString()) return source;
        std::string code = embed::toUtf8(source);
        return evalScriptDirect(code, EvalOptions{.filename = "<eval>"});
    };

    auto funcHook = [](runtime::DynamicFunctionKind kind, std::span<const Value> args) -> Value {
        return evalFunction(kind, args);
    };

    runtime::rtSetDefaultDynamicEvalHost(evalHook);
    runtime::rtSetDefaultDynamicFunctionHost(funcHook);
    embed::setDynamicEvalHook(evalHook);
    embed::setDynamicFunctionHook(funcHook);
}

}  // namespace bronze::eval
