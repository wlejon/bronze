#include "eval/eval.h"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

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

static std::vector<std::unique_ptr<BrassTieredProgram>>& retainedPrograms() {
    static auto* list = new std::vector<std::unique_ptr<BrassTieredProgram>>();
    return *list;
}
static std::mutex g_programsMutex;
static std::mutex g_jitCompileMutex;
static std::atomic<uint64_t> s_evalCounter{0};
static std::atomic<ExecutionTier> s_defaultTier{ExecutionTier::Auto};

// At exit, no retained program may still be compiling in the background:
// its worker would be running brass while the process tears brass's
// statics down. Queued compiles are dropped and the one in flight finishes.
void stopRetainedBackgroundCompiles() {
    std::lock_guard<std::mutex> lock(g_programsMutex);
    for (auto& program : retainedPrograms()) program->stopBackgroundCompilation();
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

// The one compile path: the program lowered to IL, then handed to the
// tiered engine at the options' tier (the process default when unset).
std::unique_ptr<BrassTieredProgram> compileAst(
    std::unique_ptr<ast::Module> astModule,
    const EvalOptions& options,
    const std::string& resName,
    DiagnosticSink& diags,
    SourceSet& sources) {

    if (!astModule) return nullptr;
    std::lock_guard<std::mutex> compileLock(g_jitCompileMutex);
    transformEvalAst(*astModule, resName);

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

    types::PinManifest pins;
    if (!options.pinsPath.empty()) {
        std::ifstream in(options.pinsPath, std::ios::binary);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            std::string err;
            pins.parse(ss.str(), options.pinsPath, err, /*allowObserved=*/true);
        }
    }

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
    config.tier = options.tier.value_or(defaultTier());
    config.entrySymbol = entrySym;
    config.hostGlobals = hostGlobals;
    config.optimize = options.optimize;
    config.propagateExceptionsInEntry = true;
    config.emitDebugInfo = options.emitDebugInfo;

    auto program = BrassTieredEngine(config).compile(*ilModule, diags);
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
    std::unique_ptr<BrassTieredProgram> program,
    const std::string& resName,
    embed::ModuleHandle* moduleHandleOut) {

    auto* programPtr = program.get();
    retainProgram(std::move(program));

    if (runtime::rtExceptionPending()) {
        runtime::rtClearException();
    }

    // The bracket a host asked for: opened immediately before the entry, so
    // the spans the entry registers carry the handle, and closed after the
    // microtask checkpoint, so a later program's registrations do not. The
    // handle is written even when the entry throws — a top level that got
    // halfway registered its spans on the way, and they are still the host's
    // to unload.
    const embed::ModuleHandle handle = moduleHandleOut ? embed::beginModuleLoad() : 0;
    if (moduleHandleOut) *moduleHandleOut = handle;

    uint64_t entryBits = 0;
    {
        bronze::ShadowStackFrame stackFrame;
        entryBits = programPtr->run().as_u64();
    }
    embed::drainMicrotasks();
    embed::endModuleLoad(handle);

    if (runtime::rtExceptionPending()) {
        Value thrown(runtime::rtTls()->exception_cell);
        runtime::rtClearException();
        return embed::CallResult{thrown, /*thrown=*/true};
    }

    Value entryVal(entryBits);
    if (entryVal.isObject() && embed::isPromise(entryVal)) {
        return embed::CallResult{entryVal, /*thrown=*/false};
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
    if (cr.thrown) {
        runtime::rtTls()->exception_cell = cr.value.rawBits();
        return Value::fromUndefined();
    }

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

void retainProgram(std::unique_ptr<BrassTieredProgram> program) {
    if (!program) return;
    static std::once_flag atExitOnce;
    std::call_once(atExitOnce, [] { std::atexit(&stopRetainedBackgroundCompiles); });
    std::lock_guard<std::mutex> lock(g_programsMutex);
    retainedPrograms().push_back(std::move(program));
}

void clearRetainedPrograms() {
    std::lock_guard<std::mutex> lock(g_programsMutex);
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
        runtime::rtThrowSyntaxError(err);
        Value syntaxErr(runtime::rtTls()->exception_cell);
        runtime::rtClearException();
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
    if (cr.thrown) {
        runtime::rtTls()->exception_cell = cr.value.rawBits();
        return Value::fromUndefined();
    }
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
