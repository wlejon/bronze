#include "cli/driver.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "ast/dump.h"
#include "cli/link.h"
#include "cli/link_order.h"
#include "cli/native_manifest_resolve.h"
#include "cli/usage.h"
#include "codegen/backend.h"
#include "codegen-brass/brass_backend.h"
#include "il/il.h"
#include "il/print.h"
#include "lex/lexer.h"
#include "lower/infer_stats.h"
#include "lower/lower.h"
#include "modules/modules.h"
#include "parse/parser.h"
#include "support/diagnostics.h"
#include "support/timings.h"
#include "types/dump.h"
#include "types/infer.h"

namespace bronze::cli {
namespace {

// Wall time per compilation phase, printed to stderr on `--timings`.
//
// This is the one thing bronze prints that cannot be deterministic, and the
// house rule is about bronze's OWN output — so it is opt-in, it goes to stderr,
// and nothing in the suite compares it. The alternative was measuring from
// outside with a stopwatch, which gives one number for a five-phase pipeline
// and cannot say which phase to attack.
class PhaseTimer {
public:
    explicit PhaseTimer(bool enabled) : enabled_(enabled) {
        if (enabled_) start_ = last_ = std::chrono::steady_clock::now();
    }

    void mark(const char* phase) {
        if (!enabled_) return;
        const auto now = std::chrono::steady_clock::now();
        std::fprintf(stderr, "  %-14s %8.1f ms\n", phase, millisSince(last_, now));
        last_ = now;
    }

    void total() {
        if (!enabled_) return;
        const auto now = std::chrono::steady_clock::now();
        std::fprintf(stderr, "  %-14s %8.1f ms\n", "total", millisSince(start_, now));
    }

private:
    using Clock = std::chrono::steady_clock;
    static double millisSince(Clock::time_point from, Clock::time_point to) {
        return std::chrono::duration<double, std::milli>(to - from).count();
    }

    bool enabled_;
    Clock::time_point start_{};
    Clock::time_point last_{};
};

int fail(const std::string& message) {
    std::fputs(message.c_str(), stderr);
    return 1;
}

// Diagnostics from a compilation that SUCCEEDED — warnings, since an error
// would have taken an early return. They go to stderr because stdout is the
// artefact (the IL dump, the type dump) and a caller pipes it. Rendering them
// only on failure, as the error path does, drops every annotation warning a
// discarded hint emits — and a diagnostic nobody prints is not a diagnostic.
void reportWarnings(const DiagnosticSink& diags, const SourceSet& sources) {
    if (diags.all().empty()) return;
    std::fputs(diags.render(sources).c_str(), stderr);
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// The `--host-globals` manifest: one JavaScript identifier per line, `#`
// starts a comment, blank lines ignored. Validation is strict and ASCII —
// IdentifierStart [A-Za-z_$], IdentifierPart adds digits — which is narrower
// than the lexer's own identifier grammar on purpose: the manifest is a
// contract between two builds (this one and the host's registration calls),
// and a contract is the wrong place for Unicode spellings two editors can
// disagree about. A name outside the envelope is a hard error naming the
// line, never a silent skip.
bool loadHostGlobals(const std::string& path, std::vector<std::string>& out, std::string& err) {
    std::string text;
    if (!readFile(path, text)) {
        err = "error: cannot read host-globals manifest " + path + "\n";
        return false;
    }
    std::istringstream lines(text);
    std::string line;
    int lineNo = 0;
    while (std::getline(lines, line)) {
        ++lineNo;
        if (auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        // Trim: the manifest is hand-written, and trailing whitespace (or a
        // \r from a CRLF editor) must not turn a valid name into an error.
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        const auto last = line.find_last_not_of(" \t\r");
        std::string name = line.substr(first, last - first + 1);

        auto isStart = [](char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '$';
        };
        auto isPart = [&](char c) { return isStart(c) || (c >= '0' && c <= '9'); };
        bool valid = isStart(name[0]);
        for (size_t i = 1; valid && i < name.size(); ++i) valid = isPart(name[i]);
        if (!valid) {
            err = "error: " + path + ":" + std::to_string(lineNo) +
                  ": not a valid identifier in host-globals manifest: '" + name + "'\n";
            return false;
        }
        out.push_back(std::move(name));
    }
    return true;
}

// The `--pins` manifest. The grammar and what an entry promises are in
// types/pins.h; this reads the file and reports an unreadable one through the
// same path a bad host-globals manifest takes.
bool loadPins(const std::string& path, types::PinManifest& out, std::string& err,
              bool allowObserved) {
    std::string text;
    if (!readFile(path, text)) {
        err = "error: cannot read pin manifest " + path + "\n";
        return false;
    }
    return out.parse(text, path, err, allowObserved);
}

// Does this invocation have a HOST BOUNDARY — a channel through which a value
// no part of the compiled text ever built can reach the compiled code?
//
// There are exactly three, and each is named on the command line: a
// `--host-globals` manifest (the host registers whatever those names hold),
// `--emit-obj` (the host's own link step puts the object beside its own code
// and calls the exported entry with whatever it likes), and `--emit-shared`
// (the same, resolved at run time). Any of them and the compiled program is
// half a program. None of them and `bronze build` linked the WHOLE of it: the
// only values that can reach an operator are the ones the module graph's own
// text builds, and a whole-program scan of that text is a proof rather than a
// guess.
//
// One predicate rather than three tests spelled out at each use, because the
// answer is what a promise flag is FOR: `--assume-no-bigint` asserts something
// about exactly the part of the program this predicate says the compiler
// cannot see, so a build for which it answers false needs no promise at all.
bool hasHostBoundary(const std::string& hostGlobalsPath, bool emitObj, bool emitShared) {
    return !hostGlobalsPath.empty() || emitObj || emitShared;
}

}  // namespace

int runTypes(const std::string& sourcePath, std::string* outString,
             const std::vector<modules::ModuleRoot>& moduleRoots,
             const std::string& importMapPath) {
    SourceSet sources;
    DiagnosticSink diags;
    auto astModule = modules::loadProgram(sourcePath, sources, diags, {moduleRoots, importMapPath});
    if (!astModule) {
        std::string msg = diags.render(sources);
        if (outString) *outString = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    auto inferred = types::inferModule(*astModule, diags);
    if (diags.hasErrors() || !inferred) {
        std::string msg = diags.render(sources);
        if (outString) *outString = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    // The loader's warnings — a dynamic import() it did not follow, a dropped
    // annotation — are as much this command's answer as the types are.
    reportWarnings(diags, sources);
    std::string printed = types::dump(*inferred);
    if (outString) {
        *outString = printed;
    } else {
        std::fputs(printed.c_str(), stdout);
    }
    return 0;
}

int runIl(const std::string& sourcePath, std::string* outString, bool infer,
          const std::string& hostGlobalsPath,
          const std::vector<modules::ModuleRoot>& moduleRoots,
          const std::string& importMapPath, bool inferStats,
          bool assumeNoBigInt, const std::string& pinsPath, const std::string& censusOutPath,
          bool pinsAllowObserved, const std::string& nativeManifestPath,
          const std::string& nativeLibPath) {
    // The manifests are read before any compilation happens: an unreadable file
    // or a bad line is a fact about the INVOCATION, and burying it after a
    // long compile would report it as late as possible for no reason.
    std::vector<std::string> hostGlobals;
    if (!hostGlobalsPath.empty()) {
        std::string err;
        if (!loadHostGlobals(hostGlobalsPath, hostGlobals, err)) {
            if (outString) *outString = err;
            else std::fputs(err.c_str(), stderr);
            return 1;
        }
    }
    std::string nativeManifestErr;
    auto nativeManifest = resolveNativeManifest(nativeManifestPath, nativeLibPath, nativeManifestErr);
    if (!nativeManifestErr.empty()) {
        if (outString) *outString = nativeManifestErr;
        else std::fputs(nativeManifestErr.c_str(), stderr);
        return 1;
    }
    if (nativeManifest) {
        for (const auto& root : nativeManifest->namespaceRoots()) {
            hostGlobals.push_back(root);
        }
        for (const auto& cls : nativeManifest->knownClasses()) {
            hostGlobals.push_back(cls);
        }
    }
    types::PinManifest pins;
    if (!pinsPath.empty()) {
        std::string err;
        if (!loadPins(pinsPath, pins, err, pinsAllowObserved)) {
            if (outString) *outString = err;
            else std::fputs(err.c_str(), stderr);
            return 1;
        }
    }

    // The graph — resolution, loading, linking — is `src/modules`' job; the
    // CLI is a composition root and stays one. What comes back is the single
    // merged AST module every later stage already understands.
    SourceSet sources;
    DiagnosticSink diags;
    auto astModule = modules::loadProgram(sourcePath, sources, diags, {moduleRoots, importMapPath});
    if (!astModule) {
        std::string msg = diags.render(sources);
        if (outString) *outString = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    // The CLI runs inference and hands the side table to lowering. A null side
    // table is the no-inference mode, not a failure mode — inference itself
    // only ever fails on an internal impossibility, which is diagnosed and
    // fatal.
    std::optional<types::InferenceResult> inferred;
    if (infer) {
        inferred = types::inferModule(*astModule, diags,
                                      hostGlobals.empty() ? nullptr : &hostGlobals,
                                      pins.empty() ? nullptr : &pins);
        if (diags.hasErrors() || !inferred) {
            std::string msg = diags.render(sources);
            if (outString) *outString = msg;
            else std::fputs(msg.c_str(), stderr);
            return 1;
        }
    }

    // `&sources` is not optional here even though this path only PRINTS the
    // IL: `import.meta` resolves its module's URL out of the source set at
    // lowering time, so a null one would make `bronze il` dump a different
    // program from the one `bronze build` compiles.
    // `bronze il` names no output kind, so its only boundary is a manifest.
    // Deciding it the same way `runBuild` does is the point of the command:
    // the dump has to be the IL the executable would compile, and a `mul` that
    // read `dynamic` here and `f64` there would make it a different program.
    const bool noBigIntPromised =
        assumeNoBigInt ||
        !hasHostBoundary(hostGlobalsPath, /*emitObj=*/false, /*emitShared=*/false);

    lower::InferStatsCollector statsCollector;
    auto ilModule = lower::lowerModule(*astModule, diags,
                                       inferred ? &*inferred : nullptr,
                                       hostGlobals.empty() ? nullptr : &hostGlobals,
                                       &sources,
                                       inferStats ? &statsCollector : nullptr,
                                       noBigIntPromised,
                                       pins.empty() ? nullptr : &pins,
                                       censusOutPath,
                                       nativeManifest ? &*nativeManifest : nullptr);
    if (diags.hasErrors() || !ilModule) {
        std::string msg = diags.render(sources);
        if (outString) *outString = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    reportWarnings(diags, sources);
    // The statistics before the dump, not after it: this command's artefact is
    // megabytes of IL on a real library, and a report that has to be found at
    // the end of it is a report nobody reads. Both are deterministic, so the
    // order is a convenience and not a contract.
    std::string printed = inferStats ? statsCollector.format() : std::string();
    printed += il::print(*ilModule);
    if (outString) {
        *outString = printed;
    } else {
        std::fputs(printed.c_str(), stdout);
    }
    return 0;
}

int runBuild(const std::string& sourcePath, const std::string& outputPath, std::string* errOut,
             bool infer, bool timings, bool emitObj, const std::string& hostGlobalsPath,
             bool inferStats, std::string* statsOut,
             const std::vector<modules::ModuleRoot>& moduleRoots,
             const std::string& entrySymbol, bool emitShared, bool retainFnSource,
             const std::string& importMapPath, bool assumeNoBigInt,
             const std::string& pinsPath, const std::string& censusOutPath,
             bool pinsAllowObserved, const std::string& nativeManifestPath,
             const std::string& nativeLibPath, const std::string& entryResolvesAs) {
    // Two output kinds, named on one command line: a fact about the
    // INVOCATION, so it is refused here, before anything is read or compiled,
    // and it names both flags rather than silently letting one win.
    if (emitObj && emitShared) {
        std::string msg =
            "error: --emit-obj and --emit-shared name two different outputs "
            "(an object for a host's own link step, and a linked loadable module); "
            "pass one\n";
        if (errOut) *errOut = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    // The backend reports the inside of its own phase, and it is reached
    // through `codegen::Backend`, which no debugging concern belongs in.
    support::setTimingsEnabled(timings);
    PhaseTimer timer(timings);

    // Before any compilation, for the reason runIl gives: a bad manifest is a
    // fact about the invocation.
    std::vector<std::string> hostGlobals;
    if (!hostGlobalsPath.empty()) {
        std::string manifestErr;
        if (!loadHostGlobals(hostGlobalsPath, hostGlobals, manifestErr)) {
            if (errOut) *errOut = manifestErr;
            else std::fputs(manifestErr.c_str(), stderr);
            return 1;
        }
    }
    std::string nativeManifestErr;
    auto nativeManifest = resolveNativeManifest(nativeManifestPath, nativeLibPath, nativeManifestErr);
    if (!nativeManifestErr.empty()) {
        if (errOut) *errOut = nativeManifestErr;
        else std::fputs(nativeManifestErr.c_str(), stderr);
        return 1;
    }
    if (nativeManifest) {
        for (const auto& root : nativeManifest->namespaceRoots()) {
            hostGlobals.push_back(root);
        }
        for (const auto& cls : nativeManifest->knownClasses()) {
            hostGlobals.push_back(cls);
        }
    }
    types::PinManifest pins;
    if (!pinsPath.empty()) {
        std::string manifestErr;
        if (!loadPins(pinsPath, pins, manifestErr, pinsAllowObserved)) {
            if (errOut) *errOut = manifestErr;
            else std::fputs(manifestErr.c_str(), stderr);
            return 1;
        }
    }

    SourceSet sources;
    DiagnosticSink diags;
    modules::ModuleOptions moduleOptions{moduleRoots, importMapPath};
    moduleOptions.entryResolvesAs = entryResolvesAs;
    auto astModule = modules::loadProgram(sourcePath, sources, diags, moduleOptions);
    timer.mark("load");
    if (!astModule) {
        std::string msg = diags.render(sources);
        if (errOut) *errOut = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    std::optional<types::InferenceResult> inferred;
    if (infer) {
        inferred = types::inferModule(*astModule, diags,
                                      hostGlobals.empty() ? nullptr : &hostGlobals,
                                      pins.empty() ? nullptr : &pins);
        timer.mark("infer");
        if (diags.hasErrors() || !inferred) {
            std::string msg = diags.render(sources);
            if (errOut) *errOut = msg;
            else std::fputs(msg.c_str(), stderr);
            return 1;
        }
    }

    // A standalone executable has no host boundary, so the promise is already
    // true of it and the whole-program scan inside `Lowerer` is the only thing
    // left to satisfy. A build that HAS a boundary keeps needing the flag: the
    // values crossing it are the ones no scan can see.
    const bool noBigIntPromised =
        assumeNoBigInt || !hasHostBoundary(hostGlobalsPath, emitObj, emitShared);

    lower::InferStatsCollector statsCollector;
    auto ilModule = lower::lowerModule(*astModule, diags,
                                       inferred ? &*inferred : nullptr,
                                       hostGlobals.empty() ? nullptr : &hostGlobals,
                                       &sources,
                                       inferStats ? &statsCollector : nullptr,
                                       noBigIntPromised,
                                       pins.empty() ? nullptr : &pins,
                                       censusOutPath,
                                       nativeManifest ? &*nativeManifest : nullptr);
    timer.mark("lower");
    if (diags.hasErrors() || !ilModule) {
        std::string msg = diags.render(sources);
        if (errOut) *errOut = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    // Dropped AFTER lowering rather than never gathered, because the ranges on
    // each function are what lowering produced and the texts are what they
    // index: clearing one place is the whole of the flag, and nothing
    // downstream has to be told about it. The backend emits a blob only for a
    // file it still has.
    if (!retainFnSource) ilModule->sourceTexts.clear();

    if (inferStats) {
        std::string statsStr = statsCollector.format();
        if (statsOut) {
            *statsOut = statsStr;
        } else {
            std::fputs(statsStr.c_str(), stdout);
        }
    }

    reportWarnings(diags, sources);

    // `--emit-obj`: same pipeline, stopped after object emission. The output
    // path is used exactly as given — the host's build owns naming and layout
    // — and neither linkExecutable nor the bronze_rt.lib search runs, because
    // linking is the one step that belongs to the HOST's toolchain when the
    // object is destined for embedding.
    if (emitObj) {
        BrassBackend objBackend;
        if (!entrySymbol.empty()) objBackend.setEntrySymbol(entrySymbol);
        objBackend.setHostGlobals(hostGlobals);
        const bool emittedObj = objBackend.emitObject(*ilModule, outputPath, diags);
        timer.mark("codegen");
        timer.total();
        if (!emittedObj) {
            std::string msg = diags.render(sources);
            if (errOut) *errOut = msg;
            else std::fputs(msg.c_str(), stderr);
            return 1;
        }
        return 0;
    }

    // Everything else — an executable, and `--emit-shared`'s loadable module —
    // is the same object emission followed by a different link.
    std::filesystem::path tempObj = uniqueTempObjPath(sourcePath);

    BrassBackend backend;
    if (!entrySymbol.empty()) backend.setEntrySymbol(entrySymbol);
    backend.setHostGlobals(hostGlobals);
    backend.setSharedRuntime(emitShared);
    // Handing the backend somewhere to record its outputs is what ALLOWS it
    // to emit a large module as parallel partition objects; the temp path is
    // then the stem the partition files are named from.
    std::vector<std::string> objPaths;
    backend.setEmittedPathsOut(&objPaths);
    const bool emitted = backend.emitObject(*ilModule, tempObj.string(), diags);
    timer.mark("codegen");
    if (!emitted) {
        std::error_code ec;
        if (std::filesystem::exists(tempObj, ec)) std::filesystem::remove(tempObj, ec);
        std::string msg = diags.render(sources);
        if (errOut) *errOut = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    // Before the link, not after: a link that fails is exactly when the
    // objects are worth having, and the retention is a property of the
    // EMISSION rather than of what was done with it.
    if (!keptObjectDir().empty()) {
        const std::string retainErr = retainObjects(objPaths);
        if (!retainErr.empty()) {
            if (errOut) *errOut = retainErr;
            else std::fputs(retainErr.c_str(), stderr);
            return 1;
        }
    }

    std::vector<std::string> linkInputs = objPaths;
    if (nativeManifest) {
        for (const auto& lib : nativeManifest->extraLibPaths()) {
            linkInputs.push_back(lib);
        }
    }

    bool linked = emitShared ? linkSharedModule(linkInputs, outputPath, diags, entrySymbol)
                             : linkExecutable(linkInputs, outputPath, diags);
    timer.mark("link");
    timer.total();

    std::error_code ec;
    if (std::filesystem::exists(tempObj, ec)) {
        std::filesystem::remove(tempObj, ec);
    }
    for (const std::string& obj : objPaths) {
        std::filesystem::path p(obj);
        if (std::filesystem::exists(p, ec)) std::filesystem::remove(p, ec);
    }

    if (!linked) {
        std::string msg = diags.hasErrors() ? diags.render(sources) : "error: linking failed\n";
        if (errOut) *errOut = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    return 0;
}

int runDriver(int argc, char** argv) {
    if (argc < 2) return fail(kUsage);
    const std::string command = argv[1];

    // `bronze --help` and `bronze <command> --help` both answer with the usage
    // text, on stdout, exit 0. Every command takes a file path in the position
    // --help lands in, so without this the first thing a new user saw was
    // "error: cannot read --help".
    if (command == "--help" || command == "-h" || command == "help") {
        std::fputs(kUsage, stdout);
        return 0;
    }
    if (argc >= 3) {
        const std::string_view second = argv[2];
        if (second == "--help" || second == "-h") {
            std::fputs(kUsage, stdout);
            return 0;
        }
    }

    if (command == "version") {
#if defined(NDEBUG)
        std::puts("bronze 0.1.0 (Release)");
#else
        std::puts("bronze 0.1.0 (Debug)");
#endif
        return 0;
    }

    if (command == "link") return runLink(argc, argv);

    if (command == "lex" || command == "parse") {
        if (argc < 3) return fail("error: missing <file>\n");
        std::string text;
        if (!readFile(argv[2], text)) return fail(std::string("error: cannot read ") + argv[2] + "\n");
        SourceBuffer buffer(argv[2], std::move(text));
        DiagnosticSink diags;
        auto tokens = Lexer(buffer, diags).lex();
        if (diags.hasErrors()) return fail(diags.render(buffer));

        if (command == "lex") {
            for (const auto& t : tokens) {
                std::printf("%s\t%.*s\n", tokenKindName(t.kind),
                            static_cast<int>(t.text.size()), t.text.data());
            }
            return 0;
        }

        auto module = Parser(std::move(tokens), diags).parseModule(argv[2]);
        if (diags.hasErrors() || !module) return fail(diags.render(buffer));
        std::fputs(ast::dump(*module).c_str(), stdout);
        return 0;
    }

    if (command == "types") {
        if (argc < 3) return fail("error: missing <file>\n");
        std::string sourcePath;
        std::string importMapPath;
        std::vector<modules::ModuleRoot> moduleRoots;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--module-root") {
                if (i + 1 < argc) {
                    std::string val = argv[++i];
                    size_t eq = val.find('=');
                    if (eq == std::string::npos) return fail("error: --module-root requires <prefix>=<path>\n");
                    moduleRoots.push_back({val.substr(0, eq), val.substr(eq + 1)});
                } else {
                    return fail("error: missing argument for --module-root\n");
                }
            } else if (arg.rfind("--module-root=", 0) == 0) {
                std::string val = arg.substr(14);
                size_t eq = val.find('=');
                if (eq == std::string::npos) return fail("error: --module-root requires <prefix>=<path>\n");
                moduleRoots.push_back({val.substr(0, eq), val.substr(eq + 1)});
            } else if (arg == "--import-map") {
                if (i + 1 < argc) {
                    importMapPath = argv[++i];
                } else {
                    return fail("error: missing argument for --import-map\n");
                }
            } else if (arg.rfind("--import-map=", 0) == 0) {
                importMapPath = arg.substr(13);
                if (importMapPath.empty()) return fail("error: missing argument for --import-map\n");
            } else if (sourcePath.empty()) {
                sourcePath = arg;
            } else {
                return fail("error: unexpected argument " + arg + "\n");
            }
        }
        if (sourcePath.empty()) return fail("error: missing <file>\n");
        return runTypes(sourcePath, nullptr, moduleRoots, importMapPath);
    }

    if (command == "il") {
        if (argc < 3) return fail("error: missing <file>\n");
        std::string sourcePath;
        std::string hostGlobalsPath;
        std::string importMapPath;
        std::vector<modules::ModuleRoot> moduleRoots;
        bool infer = true;
        bool inferStats = false;
        bool assumeNoBigInt = false;
        std::string pinsPath;
        std::string censusOutPath;
        bool pinsAllowObserved = false;
        std::string nativeManifestPath;
        std::string nativeLibPath;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--no-infer") {
                infer = false;
            } else if (arg == "--infer-stats") {
                inferStats = true;
            } else if (arg == "--assume-no-bigint") {
                assumeNoBigInt = true;
            } else if (arg == "--host-globals") {
                if (i + 1 < argc) {
                    hostGlobalsPath = argv[++i];
                } else {
                    return fail("error: missing argument for --host-globals\n");
                }
            } else if (arg == "--native-manifest") {
                if (i + 1 < argc) {
                    nativeManifestPath = argv[++i];
                } else {
                    return fail("error: missing argument for --native-manifest\n");
                }
            } else if (arg.rfind("--native-manifest=", 0) == 0) {
                nativeManifestPath = arg.substr(18);
                if (nativeManifestPath.empty()) return fail("error: missing argument for --native-manifest\n");
            } else if (arg == "--native-lib") {
                if (i + 1 < argc) {
                    nativeLibPath = argv[++i];
                } else {
                    return fail("error: missing argument for --native-lib\n");
                }
            } else if (arg.rfind("--native-lib=", 0) == 0) {
                nativeLibPath = arg.substr(13);
                if (nativeLibPath.empty()) return fail("error: missing argument for --native-lib\n");
            } else if (arg == "--pins") {
                if (i + 1 < argc) {
                    pinsPath = argv[++i];
                } else {
                    return fail("error: missing argument for --pins\n");
                }
            } else if (arg.rfind("--pins=", 0) == 0) {
                pinsPath = arg.substr(7);
                if (pinsPath.empty()) return fail("error: missing argument for --pins\n");
            } else if (arg == "--census") {
                if (i + 1 < argc) {
                    censusOutPath = argv[++i];
                } else {
                    return fail("error: missing argument for --census\n");
                }
            } else if (arg.rfind("--census=", 0) == 0) {
                censusOutPath = arg.substr(9);
                if (censusOutPath.empty()) return fail("error: missing argument for --census\n");
            } else if (arg == "--pins-allow-observed") {
                pinsAllowObserved = true;
            } else if (arg == "--module-root") {
                if (i + 1 < argc) {
                    std::string val = argv[++i];
                    size_t eq = val.find('=');
                    if (eq == std::string::npos) return fail("error: --module-root requires <prefix>=<path>\n");
                    moduleRoots.push_back({val.substr(0, eq), val.substr(eq + 1)});
                } else {
                    return fail("error: missing argument for --module-root\n");
                }
            } else if (arg.rfind("--module-root=", 0) == 0) {
                std::string val = arg.substr(14);
                size_t eq = val.find('=');
                if (eq == std::string::npos) return fail("error: --module-root requires <prefix>=<path>\n");
                moduleRoots.push_back({val.substr(0, eq), val.substr(eq + 1)});
            } else if (arg == "--import-map") {
                if (i + 1 < argc) {
                    importMapPath = argv[++i];
                } else {
                    return fail("error: missing argument for --import-map\n");
                }
            } else if (arg.rfind("--import-map=", 0) == 0) {
                importMapPath = arg.substr(13);
                if (importMapPath.empty()) return fail("error: missing argument for --import-map\n");
            } else if (sourcePath.empty()) {
                sourcePath = arg;
            } else {
                return fail("error: unexpected argument " + arg + "\n");
            }
        }
        if (sourcePath.empty()) return fail("error: missing <file>\n");
        return runIl(sourcePath, nullptr, infer, hostGlobalsPath, moduleRoots, importMapPath,
                     inferStats, assumeNoBigInt, pinsPath, censusOutPath, pinsAllowObserved,
                     nativeManifestPath, nativeLibPath);
    }

    if (command == "build") {
        if (argc < 3) return fail("error: missing <file>\n");
        std::string sourcePath;
        std::string outputPath = "a.exe";
        std::string hostGlobalsPath;
        std::string entrySymbol;
        std::string importMapPath;
        std::vector<modules::ModuleRoot> moduleRoots;
        bool infer = true;
        bool timings = false;
        bool emitObj = false;
        bool emitShared = false;
        bool inferStats = false;
        bool retainFnSource = true;
        bool assumeNoBigInt = false;
        std::string pinsPath;
        std::string censusOutPath;
        bool pinsAllowObserved = false;
        std::string nativeManifestPath;
        std::string nativeLibPath;
        std::string linkFlagError;

        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (consumeLinkMeasurementFlag(arg, i, argc, argv, linkFlagError)) {
                if (!linkFlagError.empty()) return fail(linkFlagError);
            } else if (arg == "--no-infer") {
                infer = false;
            } else if (arg == "--no-fn-source") {
                retainFnSource = false;
            } else if (arg == "--assume-no-bigint") {
                assumeNoBigInt = true;
            } else if (arg == "--timings") {
                timings = true;
            } else if (arg == "--infer-stats") {
                inferStats = true;
            } else if (arg == "--emit-obj") {
                emitObj = true;
            } else if (arg == "--emit-shared") {
                emitShared = true;
            } else if (arg == "--entry-symbol") {
                if (i + 1 < argc) {
                    entrySymbol = argv[++i];
                } else {
                    return fail("error: missing argument for --entry-symbol\n");
                }
            } else if (arg.rfind("--entry-symbol=", 0) == 0) {
                entrySymbol = arg.substr(15);
                if (entrySymbol.empty()) {
                    return fail("error: missing argument for --entry-symbol\n");
                }
            } else if (arg == "--host-globals") {
                if (i + 1 < argc) {
                    hostGlobalsPath = argv[++i];
                } else {
                    return fail("error: missing argument for --host-globals\n");
                }
            } else if (arg == "--native-manifest") {
                if (i + 1 < argc) {
                    nativeManifestPath = argv[++i];
                } else {
                    return fail("error: missing argument for --native-manifest\n");
                }
            } else if (arg.rfind("--native-manifest=", 0) == 0) {
                nativeManifestPath = arg.substr(18);
                if (nativeManifestPath.empty()) return fail("error: missing argument for --native-manifest\n");
            } else if (arg == "--native-lib") {
                if (i + 1 < argc) {
                    nativeLibPath = argv[++i];
                } else {
                    return fail("error: missing argument for --native-lib\n");
                }
            } else if (arg.rfind("--native-lib=", 0) == 0) {
                nativeLibPath = arg.substr(13);
                if (nativeLibPath.empty()) return fail("error: missing argument for --native-lib\n");
            } else if (arg == "--pins") {
                if (i + 1 < argc) {
                    pinsPath = argv[++i];
                } else {
                    return fail("error: missing argument for --pins\n");
                }
            } else if (arg.rfind("--pins=", 0) == 0) {
                pinsPath = arg.substr(7);
                if (pinsPath.empty()) return fail("error: missing argument for --pins\n");
            } else if (arg == "--census") {
                if (i + 1 < argc) {
                    censusOutPath = argv[++i];
                } else {
                    return fail("error: missing argument for --census\n");
                }
            } else if (arg.rfind("--census=", 0) == 0) {
                censusOutPath = arg.substr(9);
                if (censusOutPath.empty()) return fail("error: missing argument for --census\n");
            } else if (arg == "--pins-allow-observed") {
                pinsAllowObserved = true;
            } else if (arg == "--module-root") {
                if (i + 1 < argc) {
                    std::string val = argv[++i];
                    size_t eq = val.find('=');
                    if (eq == std::string::npos) return fail("error: --module-root requires <prefix>=<path>\n");
                    moduleRoots.push_back({val.substr(0, eq), val.substr(eq + 1)});
                } else {
                    return fail("error: missing argument for --module-root\n");
                }
            } else if (arg.rfind("--module-root=", 0) == 0) {
                std::string val = arg.substr(14);
                size_t eq = val.find('=');
                if (eq == std::string::npos) return fail("error: --module-root requires <prefix>=<path>\n");
                moduleRoots.push_back({val.substr(0, eq), val.substr(eq + 1)});
            } else if (arg == "--import-map") {
                if (i + 1 < argc) {
                    importMapPath = argv[++i];
                } else {
                    return fail("error: missing argument for --import-map\n");
                }
            } else if (arg.rfind("--import-map=", 0) == 0) {
                importMapPath = arg.substr(13);
                if (importMapPath.empty()) return fail("error: missing argument for --import-map\n");
            } else if (arg == "-o") {
                if (i + 1 < argc) {
                    outputPath = argv[++i];
                } else {
                    return fail("error: missing argument for -o\n");
                }
            } else if (sourcePath.empty()) {
                sourcePath = arg;
            } else {
                return fail("error: unexpected argument " + arg + "\n");
            }
        }

        if (sourcePath.empty()) return fail("error: missing <file>\n");
        return runBuild(sourcePath, outputPath, nullptr, infer, timings, emitObj,
                        hostGlobalsPath, inferStats, nullptr, moduleRoots, entrySymbol,
                        emitShared, retainFnSource, importMapPath, assumeNoBigInt, pinsPath,
                        censusOutPath, pinsAllowObserved, nativeManifestPath, nativeLibPath);
    }

    return fail(kUsage);
}

}  // namespace bronze::cli
