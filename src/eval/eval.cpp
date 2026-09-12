#include "eval/eval.h"

#include <atomic>
#include <mutex>
#include <vector>

#include "ast/ast.h"
#include "il/print.h"
#include "codegen-brass/brass_backend.h"
#include "codegen-brass/brass_jit.h"
#include "embed/embed.h"
#include "lex/lexer.h"
#include "lower/lower.h"
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

namespace bronze::eval {

namespace {

static std::vector<std::unique_ptr<BrassJitProgram>> g_retainedJitPrograms;
static std::mutex g_programsMutex;
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
            if (vd->isVar && !vd->name.empty()) {
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
            if (!fd->name.empty()) {
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

std::unique_ptr<BrassJitProgram> compileSourceToJit(
    const std::string& code,
    const EvalOptions& options,
    const std::string& resName,
    DiagnosticSink& diags,
    SourceSet& sources) {

    const auto& buffer = sources.add(options.filename, code);
    Lexer lexer(buffer, diags);
    auto tokens = lexer.lex();
    if (diags.hasErrors()) return nullptr;

    Parser parser(std::move(tokens), diags);
    auto astModule = parser.parseModule(options.filename);
    if (diags.hasErrors() || !astModule) return nullptr;

    transformEvalAst(*astModule, resName);

    std::vector<std::string> hostGlobals = options.hostGlobals;
    if (hostGlobals.empty()) {
        for (const auto& entry : runtime::rtHostGlobalEntries()) {
            hostGlobals.push_back(entry.first);
        }
    }

    auto inferred = types::inferModule(*astModule, diags,
                                       hostGlobals.empty() ? nullptr : &hostGlobals);
    if (diags.hasErrors() || !inferred) return nullptr;

    auto ilModule = lower::lowerModule(*astModule, diags,
                                       inferred ? &*inferred : nullptr,
                                       hostGlobals.empty() ? nullptr : &hostGlobals,
                                       &sources);
    if (diags.hasErrors() || !ilModule) return nullptr;

    if (!options.retainSource) {
        ilModule->sourceTexts.clear();
    }

    const uint64_t evalId = s_evalCounter.fetch_add(1, std::memory_order_relaxed);
    const std::string entrySym = "__bronze_dyn_entry_" + std::to_string(evalId);

    BrassBackend backend;
    backend.setEntrySymbol(entrySym);
    backend.setHostGlobals(hostGlobals);

    return backend.compileToJit(*ilModule, diags);
}

}  // namespace

void retainJitProgram(std::unique_ptr<BrassJitProgram> program) {
    if (!program) return;
    std::lock_guard<std::mutex> lock(g_programsMutex);
    g_retainedJitPrograms.push_back(std::move(program));
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

    auto* programPtr = jitProgram.get();
    retainJitProgram(std::move(jitProgram));

    if (runtime::rtExceptionPending()) {
        runtime::rtClearException();
    }

    using RawEntryFn = uint64_t (*)();
    auto entryFn = reinterpret_cast<RawEntryFn>(programPtr->entryPoint());
    uint64_t entryBits = entryFn ? entryFn() : 0;
    embed::drainMicrotasks();

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
    embed::setDynamicEvalHook([](Value source) -> Value {
        if (!source.isString()) return source;
        std::string code = embed::toUtf8(source);
        return evalScriptDirect(code, EvalOptions{.filename = "<eval>"});
    });

    embed::setDynamicFunctionHook([](runtime::DynamicFunctionKind kind, std::span<const Value> args) -> Value {
        return evalFunction(kind, args);
    });
}

}  // namespace bronze::eval
