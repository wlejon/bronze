#include "eval/eval.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <brass/runtime/bytecode_warmer.hpp>
#include <brass/runtime/code_installer.hpp>
#include <brass/runtime/compile_pool.hpp>
#include <brass/runtime/multi_tier_pipeline.hpp>

#include "ast/ast.h"
#include "il/print.h"
#include "codegen-brass/brass_backend.h"
#include "codegen-brass/brass_jit.h"
#include "embed/embed.h"
#include "eval/code_cache.h"
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
#include "support/timings.h"
#include "types/infer.h"
#include "types/pins.h"

namespace bronze::eval {

namespace {

// BRONZE_TIMINGS=1: an embedded compile prints its phase times to stderr the
// way `bronze build --timings` does (support/timings.h), so a host's slow
// load says which phase to attack.
bool evalTimingsEnabled() {
    static const bool on = [] {
        const char* v = std::getenv("BRONZE_TIMINGS");
        const bool enabled = v && v[0] == '1';
        if (enabled) support::setTimingsEnabled(true);
        return enabled;
    }();
    return on;
}

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
// The shared-program keys some thread is compiling now, and the condition a
// thread asking for one of them waits on. Guarded by g_jitCompileMutex.
static std::unordered_set<std::string>& sharedInFlight() {
    static auto* set = new std::unordered_set<std::string>();
    return *set;
}
static std::condition_variable& sharedInFlightCv() {
    static auto* cv = new std::condition_variable();
    return *cv;
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

// Warm lists (code_cache.h, brass/runtime/bytecode_warmer.hpp): a program
// compiled with the code cache records the order its functions first ran in
// (for kWarmWindow after the first), and the list is stored beside its entry
// when the host stops its compiles or the process exits. The next run that
// hits the entry hands the list to the program's warmer, which builds those
// functions' Tier-0 bytecode on threads of its own while the program starts.
// BRASS_BYTECODE_WARM=0 turns both off (a BRONZE_* switch would be part of
// the cache key); BRASS_BYTECODE_WARM_THREADS sets the warmer's threads.
constexpr std::chrono::milliseconds kWarmWindow{3000};
constexpr size_t kWarmMaxNames = 20000;

struct WarmRecord {
    std::weak_ptr<BrassTieredProgram> program;
    const BrassTieredProgram* raw = nullptr;
    std::string dir;
    cache::Digest key;
};
static std::mutex g_warmMutex;
static std::vector<WarmRecord>& warmRecords() {
    static auto* list = new std::vector<WarmRecord>();
    return *list;
}

bool warmEnabled() {
    static const bool on = [] {
        const char* v = std::getenv("BRASS_BYTECODE_WARM");
        return !(v && v[0] == '0');
    }();
    return on;
}

unsigned warmThreads() {
    const char* v = std::getenv("BRASS_BYTECODE_WARM_THREADS");
    const int n = v ? std::atoi(v) : 0;
    return n > 0 ? static_cast<unsigned>(n) : 2u;
}

void armWarmList(const std::shared_ptr<BrassTieredProgram>& program, const std::string& dir,
                 const cache::Digest& key, bool hit) {
    if (!program || dir.empty() || program->tier() != ExecutionTier::Auto || !program->mirModule() ||
        !warmEnabled()) {
        return;
    }
    {
        // A program shared across threads comes back from each compile.
        std::lock_guard<std::mutex> lock(g_warmMutex);
        for (const WarmRecord& r : warmRecords()) {
            if (r.raw == program.get()) return;
        }
        warmRecords().push_back({program, program.get(), dir, key});
    }
    brass::runtime::BytecodeWarmer& warmer = program->dispatchTable().pipeline().bytecode_warmer();
    if (hit) {
        std::vector<std::string> names = cache::loadWarmList(dir, key);
        if (!names.empty()) warmer.warm(*program->mirModule(), std::move(names), warmThreads());
    }
    warmer.start_recording(kWarmWindow, kWarmMaxNames);
}

void saveWarmLists() {
    std::lock_guard<std::mutex> lock(g_warmMutex);
    for (const WarmRecord& r : warmRecords()) {
        std::shared_ptr<BrassTieredProgram> program = r.program.lock();
        if (!program) continue;
        brass::runtime::BytecodeWarmer& warmer = program->dispatchTable().pipeline().bytecode_warmer();
        warmer.stop();
        cache::storeWarmList(r.dir, r.key, warmer.first_use_log());
    }
    warmRecords().clear();
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

// What a compile reads besides its sources, gathered once: the key of the code
// cache and the front end both read it.
struct CompileInputs {
    std::vector<std::string> hostGlobals;
    std::optional<lower::NativeManifest> nativeManifest;
    std::string manifestJson;  // "" when the program is compiled against no natives
    bool deferNativeBind = false;
    std::string pinsText;
    types::PinManifest pins;
    ExecutionTier tier = ExecutionTier::Auto;
};

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

void reportProgress(const EvalOptions& options, const char* phase, double fraction) {
    if (options.onProgress) options.onProgress(CompileProgress{phase, fraction});
}

// The code-cache key of a compile (code_cache.h says what goes in), or none
// when the options ask for no cache or this binary cannot be identified.
std::optional<cache::Digest> codeCacheKey(const EvalOptions& options, const modules::ModuleOptions& modOpts,
                                          const CompileInputs& in, std::string_view entryText) {
    if (options.codeCacheDir.empty()) return std::nullopt;
    const std::string& process = cache::processKeyMaterial();
    if (process.empty()) return std::nullopt;
    cache::KeyBuilder k;
    k.field(process);
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

// Where a compile that missed the cache stores what it lowered.
struct CacheStore {
    const std::string* dir = nullptr;
    uint64_t maxBytes = 0;
    cache::Digest key;
    const modules::DependencyLog* deps = nullptr;
};

// The one compile path: the program lowered to IL (or the IL a cache hit
// supplies), then handed to the tiered engine at the options' tier (the
// process default when unset). With shareAcrossThreads, a program compiled
// before from the same inputs is reused (EvalOptions says when), and
// `resName` becomes its result name.
std::shared_ptr<BrassTieredProgram> compileUnit(
    std::unique_ptr<ast::Module> astModule,
    std::unique_ptr<il::Module> ilModule,
    const EvalOptions& options,
    const CompileInputs& in,
    std::string& resName,
    DiagnosticSink& diags,
    SourceSet& sources,
    CompiledScript& res,
    const CacheStore* store) {

    if (!astModule && !ilModule) return nullptr;
    // The lock guards the shared-program table, and nothing else: inference,
    // lowering and the tiered backend keep no state between compiles (lazy
    // bodies already build concurrently with other compiles), so one thread's
    // compile — a worker booting its module graph — must not stall another
    // thread's `import()`, eval or script for its whole duration. The
    // whole-program optimizing tier is the exception and stays serialized.
    std::unique_lock<std::mutex> compileLock(g_jitCompileMutex);
    const std::vector<std::string>& hostGlobals = in.hostGlobals;

    const bool share = options.shareAcrossThreads && in.tier != ExecutionTier::Tier2_Optimized;
    std::string shareKey;
    // A shared program compiles once: a second thread asking for the same key
    // while the first compiles waits for that compile, not for every compile.
    struct InFlightClaim {
        std::unique_lock<std::mutex>& lock;
        const std::string* key = nullptr;
        ~InFlightClaim() {
            if (!key) return;
            if (!lock.owns_lock()) lock.lock();
            sharedInFlight().erase(*key);
            sharedInFlightCv().notify_all();
        }
    } claim{compileLock};
    if (share) {
        shareKey = sharedProgramKey(options, in.tier, hostGlobals, in.manifestJson, in.pinsText, sources);
        sharedInFlightCv().wait(compileLock, [&] { return !sharedInFlight().count(shareKey); });
        sharedInFlight().insert(shareKey);
        claim.key = &shareKey;
        auto it = sharedPrograms().find(shareKey);
        if (it != sharedPrograms().end() && !runtime::rtThreadHasModuleInstance(it->second.program->moduleSlotCell())) {
            resName = it->second.resName;
            return it->second.program;
        }
    }
    if (in.tier != ExecutionTier::Tier2_Optimized) compileLock.unlock();
    support::PhaseTimer timer(evalTimingsEnabled(), 2);
    if (!ilModule) {
        transformEvalAst(*astModule, resName);

        auto inferred = types::inferModule(*astModule, diags,
                                           hostGlobals.empty() ? nullptr : &hostGlobals,
                                           in.pins.empty() ? nullptr : &in.pins);
        if (diags.hasErrors() || !inferred) return nullptr;
        timer.mark("infer");
        reportProgress(options, "infer", 0.60);

        auto lowered = lower::lowerModule(*astModule, diags,
                                      inferred ? &*inferred : nullptr,
                                      hostGlobals.empty() ? nullptr : &hostGlobals,
                                      &sources,
                                      /*stats=*/nullptr,
                                      /*assumeNoBigInt=*/false,
                                      in.pins.empty() ? nullptr : &in.pins,
                                      options.censusOutPath,
                                      in.nativeManifest ? &*in.nativeManifest : nullptr);
        if (diags.hasErrors() || !lowered) return nullptr;
        ilModule = std::make_unique<il::Module>(std::move(*lowered));
        timer.mark("lower");
        reportProgress(options, "lower", 0.88);

        if (store) {
            cache::store(*store->dir, store->maxBytes, store->key, resName, sources, *store->deps, *ilModule);
            timer.mark("cache store");
        }
        if (!options.retainSource) {
            ilModule->sourceTexts.clear();
        }
    }

    const uint64_t evalId = s_evalCounter.fetch_add(1, std::memory_order_relaxed);
    const std::string entrySym = "__bronze_dyn_entry_" + std::to_string(evalId);

    TieredEngineConfig config;
    config.tier = in.tier;
    config.entrySymbol = entrySym;
    config.hostGlobals = hostGlobals;
    config.emitDebugInfo = options.emitDebugInfo;

    std::shared_ptr<BrassTieredProgram> program = BrassTieredEngine(config).compile(*ilModule, diags);
    if (!program) return nullptr;
    timer.mark("codegen");
    // Bind the program's import table from the registry it was compiled
    // against, before anything runs. The entry rebinds on its own first
    // instruction and would be FATAL on a gap; doing it here first turns a
    // native unregistered between compile and run into a diagnostic naming
    // it instead. A compile against a captured manifest may be on a thread
    // with no registry at all: the running thread binds it instead
    // (runCompiledScript).
    if (!ilModule->nativeImports.empty()) {
        if (in.deferNativeBind) {
            res.deferredNativeTable = entrySym + "_native_imports";
        } else {
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
    }
    if (share) {
        if (!compileLock.owns_lock()) compileLock.lock();
        sharedPrograms()[shareKey] = SharedProgram{program, resName};
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
    if (!options.externalModules.empty() || options.externalModulesCaptured) {
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
    saveWarmLists();
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

void captureThreadInputs(EvalOptions& options) {
    if (options.hostGlobals.empty()) {
        for (const auto& entry : runtime::rtHostGlobalEntries()) options.hostGlobals.push_back(entry.first);
    }
    if (options.moduleRegistry && !options.externalModulesCaptured) {
        if (options.externalModules.empty()) options.externalModules = runtime::rtModuleRegistryPaths();
        options.externalModulesCaptured = true;
    }
    if (!options.nativeManifestJson) {
        options.nativeManifestJson = embed::hostNativeNames().empty() ? std::string() : embed::nativeManifestJson();
    }
}

namespace {

// compileScript and compileFile: the entry is `entryText`, read from `path`
// when `fromFile`. `entryKnown` is false only for a file that could not be
// read up front, which then compiles without the cache and fails as it would
// have.
std::unique_ptr<CompiledScript> compileEntry(std::string_view entryText, bool entryKnown, bool fromFile,
                                             const std::string& path, const EvalOptions& options) {
    auto res = newScript(options);
    SourceSet sources;
    DiagnosticSink diags;
    support::PhaseTimer timer(evalTimingsEnabled(), 2);
    CompileInputs in;
    if (!gatherInputs(options, in, diags)) {
        finishScript(*res, diags, sources);
        return res;
    }
    modules::ModuleOptions modOpts = moduleOptionsFor(options);
    modules::DependencyLog deps;
    const std::optional<cache::Digest> key =
        entryKnown ? codeCacheKey(options, modOpts, in, entryText) : std::nullopt;
    if (key) {
        // Deterministic, so the name the stored IL carries is the name this
        // compile would have chosen.
        res->resName = "__bronze_eval_res_c" + key->hex().substr(0, 16);
        cache::Hit hit;
        std::string why;
        if (cache::load(options.codeCacheDir, *key, entryText, modOpts.moduleRoots, options.retainSource, hit, why)) {
            timer.mark("cache load");
            reportProgress(options, "cache", 0.80);
            res->cacheStatus = CodeCacheStatus::Hit;
            res->resName = hit.resName;
            res->program = compileUnit(nullptr, std::move(hit.module), options, in, res->resName, diags,
                                       hit.sources, *res, nullptr);
            armWarmList(res->program, options.codeCacheDir, *key, /*hit=*/true);
            finishScript(*res, diags, hit.sources);
            reportProgress(options, "done", 1.0);
            return res;
        }
        res->cacheStatus = CodeCacheStatus::Miss;
        res->cacheNote = why;
        modOpts.dependencyLog = &deps;
    }
    auto astModule = fromFile ? modules::loadProgram(path, sources, diags, modOpts)
                              : modules::loadProgramSource(std::string(entryText), options.filename, sources, diags,
                                                           modOpts);
    timer.mark("load+parse");
    reportProgress(options, "load", 0.07);
    if (!diags.hasErrors() && astModule) {
        const CacheStore store{&options.codeCacheDir, options.codeCacheMaxBytes, key ? *key : cache::Digest{}, &deps};
        res->program = compileUnit(std::move(astModule), nullptr, options, in, res->resName, diags, sources, *res,
                                   key ? &store : nullptr);
        if (key) armWarmList(res->program, options.codeCacheDir, *key, /*hit=*/false);
    }
    finishScript(*res, diags, sources);
    reportProgress(options, "done", 1.0);
    return res;
}

}  // namespace

std::unique_ptr<CompiledScript> compileScript(std::string_view source, const EvalOptions& options) {
    if (source.empty()) {
        auto res = std::make_unique<CompiledScript>();
        res->success = true;
        return res;
    }
    return compileEntry(source, /*entryKnown=*/true, /*fromFile=*/false, options.filename, options);
}

std::unique_ptr<CompiledScript> compileFile(const std::string& filePath, const EvalOptions& options) {
    std::string text;
    bool known = false;
    if (!options.codeCacheDir.empty()) {
        std::ifstream in(filePath, std::ios::binary);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            text = ss.str();
            known = true;
        }
    }
    return compileEntry(text, known, /*fromFile=*/true, filePath, options);
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
    // A compile against a captured manifest left its import table to the
    // thread that runs it: bound here, from this thread's registry, before
    // the entry's first instruction needs it.
    if (!script->deferredNativeTable.empty()) {
        void* table = script->program->symbolAddress(script->deferredNativeTable);
        std::vector<std::string> missing;
        if (!table || !embed::bindNativeImports(table, &missing)) {
            std::string msg = "native imports unbound: the host registered no native for";
            for (const auto& m : missing) msg += "\n  " + m;
            if (!table) msg += "\n  (the program's import table symbol is missing)";
            Value err;
            runtime::rtTryCatch([&] { runtime::rtThrowSyntaxError(msg); }, err);
            return embed::CallResult{err, /*thrown=*/true};
        }
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
