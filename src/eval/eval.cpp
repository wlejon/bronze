#include "eval/eval.h"

#include <atomic>
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
#include "runtime/host_globals.h"
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

static std::vector<std::unique_ptr<BrassJitProgram>>& retainedPrograms() {
    static auto* list = new std::vector<std::unique_ptr<BrassJitProgram>>();
    return *list;
}
static std::mutex g_programsMutex;
static std::mutex g_jitCompileMutex;
static std::atomic<uint64_t> s_evalCounter{0};

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

std::unique_ptr<BrassJitProgram> compileAstToJit(
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

    BrassBackend backend;
    backend.setEntrySymbol(entrySym);
    backend.setHostGlobals(hostGlobals);

    auto program = backend.compileToJit(*ilModule, diags);
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

std::unique_ptr<BrassJitProgram> compileSourceToJit(
    const std::string& code,
    const EvalOptions& options,
    const std::string& resName,
    DiagnosticSink& diags,
    SourceSet& sources) {

    modules::ModuleOptions modOpts;
    modOpts.moduleRoots = options.moduleRoots;
    modOpts.entryResolvesAs = options.entryResolvesAs;

    auto astModule = modules::loadProgramSource(code, options.filename, sources, diags, modOpts);
    if (diags.hasErrors() || !astModule) return nullptr;

    return compileAstToJit(std::move(astModule), options, resName, diags, sources);
}

std::unique_ptr<BrassJitProgram> compileFileToJit(
    const std::string& filePath,
    const EvalOptions& options,
    const std::string& resName,
    DiagnosticSink& diags,
    SourceSet& sources) {

    modules::ModuleOptions modOpts;
    modOpts.moduleRoots = options.moduleRoots;
    modOpts.entryResolvesAs = options.entryResolvesAs;

    auto astModule = modules::loadProgram(filePath, sources, diags, modOpts);
    if (diags.hasErrors() || !astModule) return nullptr;

    return compileAstToJit(std::move(astModule), options, resName, diags, sources);
}

embed::CallResult runJitProgramAndCollectResult(
    std::unique_ptr<BrassJitProgram> jitProgram,
    const std::string& resName,
    embed::ModuleHandle* moduleHandleOut) {

    auto* programPtr = jitProgram.get();
    retainJitProgram(std::move(jitProgram));

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

    using RawEntryFn = uint64_t (*)();
    auto entryFn = reinterpret_cast<RawEntryFn>(programPtr->entryPoint());
    uint64_t entryBits = entryFn ? entryFn() : 0;
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
        embed::setProperty(glob.value, resName, embed::undefined());
    }

    return embed::CallResult{result.get(), /*thrown=*/false};
}

}  // namespace

void retainJitProgram(std::unique_ptr<BrassJitProgram> program) {
    if (!program) return;
    std::lock_guard<std::mutex> lock(g_programsMutex);
    retainedPrograms().push_back(std::move(program));
}

embed::CallResult evalScript(std::string_view source, const EvalOptions& options) {
    bronze::ShadowStackFrame rootFrame;
    if (source.empty()) {
        return embed::CallResult{embed::undefined(), false};
    }

    const uint64_t evalId = s_evalCounter.fetch_add(1, std::memory_order_relaxed);
    const std::string resName = "__bronze_eval_res_" + std::to_string(evalId);

    SourceSet sources;
    DiagnosticSink diags;
    std::string codeStr(source);

    auto jitProgram = compileSourceToJit(codeStr, options, resName, diags, sources);
    if (!jitProgram) {
        std::string err = diags.render(sources);
        runtime::rtThrowSyntaxError(err);
        Value syntaxErr(runtime::rtTls()->exception_cell);
        runtime::rtClearException();
        return embed::CallResult{syntaxErr, /*thrown=*/true};
    }

    return runJitProgramAndCollectResult(std::move(jitProgram), resName, options.moduleHandleOut);
}

embed::CallResult evalFile(const std::string& filePath, const EvalOptions& options) {
    bronze::ShadowStackFrame rootFrame;
    const uint64_t evalId = s_evalCounter.fetch_add(1, std::memory_order_relaxed);
    const std::string resName = "__bronze_eval_res_" + std::to_string(evalId);

    SourceSet sources;
    DiagnosticSink diags;

    auto jitProgram = compileFileToJit(filePath, options, resName, diags, sources);
    if (!jitProgram) {
        std::string err = diags.render(sources);
        runtime::rtThrowSyntaxError(err);
        Value syntaxErr(runtime::rtTls()->exception_cell);
        runtime::rtClearException();
        return embed::CallResult{syntaxErr, /*thrown=*/true};
    }

    return runJitProgramAndCollectResult(std::move(jitProgram), resName, options.moduleHandleOut);
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

    const char* prefix = "function anonymous(";
    switch (kind) {
        case runtime::DynamicFunctionKind::Generator:
            prefix = "function* anonymous(";
            break;
        case runtime::DynamicFunctionKind::Async:
            prefix = "async function anonymous(";
            break;
        case runtime::DynamicFunctionKind::AsyncGenerator:
            prefix = "async function* anonymous(";
            break;
        case runtime::DynamicFunctionKind::Ordinary:
        default:
            break;
    }

    const uint64_t fnId = s_evalCounter.fetch_add(1, std::memory_order_relaxed);
    const std::string fnName = "__bronze_dyn_fn_" + std::to_string(fnId);
    const std::string fnCode = "globalThis." + fnName + " = " + prefix + params + "\n) {\n" + body + "\n};";

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
        embed::setProperty(glob.value, fnName, embed::undefined());
    }

    return fnObj.get();
}

Value evalFunction(std::span<const std::string> params, std::string_view body,
                   runtime::DynamicFunctionKind kind) {
    std::string paramList;
    for (size_t i = 0; i < params.size(); ++i) {
        if (i > 0) paramList += ", ";
        paramList += params[i];
    }

    const char* prefix = "function anonymous(";
    switch (kind) {
        case runtime::DynamicFunctionKind::Generator:
            prefix = "function* anonymous(";
            break;
        case runtime::DynamicFunctionKind::Async:
            prefix = "async function anonymous(";
            break;
        case runtime::DynamicFunctionKind::AsyncGenerator:
            prefix = "async function* anonymous(";
            break;
        case runtime::DynamicFunctionKind::Ordinary:
        default:
            break;
    }

    const uint64_t fnId = s_evalCounter.fetch_add(1, std::memory_order_relaxed);
    const std::string fnName = "__bronze_dyn_fn_" + std::to_string(fnId);
    const std::string fnCode = "globalThis." + fnName + " = " + prefix + paramList + "\n) {\n" + std::string(body) + "\n};";

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
        embed::setProperty(glob.value, fnName, embed::undefined());
    }

    return fnObj.get();
}

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

namespace {
struct AutoInstallHooks {
    AutoInstallHooks() {
        installDefaultDynamicHooks();
    }
};
static AutoInstallHooks s_autoInstallHooks;
}

}  // namespace bronze::eval
