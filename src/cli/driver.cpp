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
#include <vector>

#include "ast/dump.h"
#include "cli/link.h"
#include "codegen/backend.h"
#if BRONZE_WITH_LLVM
#include "codegen-llvm/llvm_backend.h"
#endif
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

constexpr const char* kUsage =
    "bronze — AOT compiler for JavaScript (native-first, LLVM backend)\n"
    "\n"
    "Usage:\n"
    "  bronze lex <file>                   Tokenize and print one token per line\n"
    "  bronze parse <file>                 Parse and print the canonical AST dump\n"
    "  bronze types <file>                 Infer types and print the canonical type dump\n"
    "  bronze il <file>                    Lower to IL and print canonical IL dump\n"
    "  bronze build <file> -o <output>     Compile JS source to native executable\n"
    "  bronze version                      Print version\n"
    "\n"
    "Options (il, build):\n"
    "  --no-infer                          Skip inference: force every inferred type\n"
    "                                      to dynamic and lower on the uniform\n"
    "                                      dynamic convention. This is the bisection\n"
    "                                      seam for a suspected miscompile — right\n"
    "                                      with it and wrong without means inference\n"
    "                                      is at fault. The oracle suite runs every\n"
    "                                      case both ways and requires the same bytes\n"
    "                                      from both.\n"
    "  --host-globals <path>               Manifest of identifiers an embedding host\n"
    "                                      will register with the runtime before the\n"
    "                                      program runs: one name per line, `#`\n"
    "                                      comments, blank lines ignored. Each joins\n"
    "                                      the provided-globals set, so reads resolve\n"
    "                                      like a builtin's instead of warning and\n"
    "                                      throwing ReferenceError. A lowering-level\n"
    "                                      fact: identical with --no-infer.\n"
    "\n"
    "Options (build):\n"
    "  --timings                           Print per-phase wall time to stderr. The\n"
    "                                      one deliberately nondeterministic thing\n"
    "                                      bronze prints, which is why it is opt-in\n"
    "                                      and on stderr: no pinned output can\n"
    "                                      see it.\n"
    "  --infer-stats                       Print deterministic compile-time inference\n"
    "                                      statistics per module to stdout (property\n"
    "                                      accesses, calls, and element operations\n"
    "                                      native vs dynamic, with top bail reasons).\n"
    "  --entry-symbol <name>               Name the object exported entry point\n"
    "                                      (default bronze_main). The entry and the\n"
    "                                      ABI stamp are the only two symbols an\n"
    "                                      object exports, so distinct names here\n"
    "                                      are what let a host link more than one\n"
    "                                      compiled module into one image.\n"
    "  --emit-obj                          Stop after object emission: -o names the\n"
    "                                      object file, written exactly where given,\n"
    "                                      and no linker runs. The embedding seam —\n"
    "                                      the host build links the object against\n"
    "                                      bronze's runtime and its own code.\n"
    "  --emit-shared                       Link a loadable module (DLL/.so/.dylib)\n"
    "                                      against the SHARED bronze runtime instead\n"
    "                                      of an executable against the static one.\n"
    "                                      It exports three names, all after the\n"
    "                                      entry: <entry>, <entry>_abi_fingerprint\n"
    "                                      and <entry>_host_globals. A host opens it\n"
    "                                      at run time, checks the stamp against the\n"
    "                                      runtime's, and calls the entry. One\n"
    "                                      runtime in the process means one heap, so\n"
    "                                      a missing shared runtime is an error and\n"
    "                                      never a fall back to the static one.\n"
    "\n"
    "TS annotations are untrusted hints. One that inference does not prove is\n"
    "discarded with a warning and the value stays dynamic.\n";

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

}  // namespace

int runTypes(const std::string& sourcePath, std::string* outString,
             const std::vector<modules::ModuleRoot>& moduleRoots) {
    SourceSet sources;
    DiagnosticSink diags;
    auto astModule = modules::loadProgram(sourcePath, sources, diags, {moduleRoots});
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
          const std::vector<modules::ModuleRoot>& moduleRoots) {
    // The manifest is read before any compilation happens: an unreadable file
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

    // The graph — resolution, loading, linking — is `src/modules`' job; the
    // CLI is a composition root and stays one. What comes back is the single
    // merged AST module every later stage already understands.
    SourceSet sources;
    DiagnosticSink diags;
    auto astModule = modules::loadProgram(sourcePath, sources, diags, {moduleRoots});
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
        inferred = types::inferModule(*astModule, diags);
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
    auto ilModule = lower::lowerModule(*astModule, diags,
                                       inferred ? &*inferred : nullptr,
                                       hostGlobals.empty() ? nullptr : &hostGlobals,
                                       &sources);
    if (diags.hasErrors() || !ilModule) {
        std::string msg = diags.render(sources);
        if (outString) *outString = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    reportWarnings(diags, sources);
    std::string printed = il::print(*ilModule);
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
             const std::string& entrySymbol, bool emitShared) {
#if !BRONZE_WITH_LLVM
    (void)sourcePath;
    (void)outputPath;
    (void)infer;
    (void)timings;
    (void)emitObj;
    (void)hostGlobalsPath;
    (void)inferStats;
    (void)statsOut;
    (void)moduleRoots;
    (void)entrySymbol;
    (void)emitShared;
    std::string msg = "error: bronze build requires LLVM backend (BRONZE_WITH_LLVM=ON)\n";
    if (errOut) *errOut = msg;
    else std::fputs(msg.c_str(), stderr);
    return 1;
#else
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

    SourceSet sources;
    DiagnosticSink diags;
    auto astModule = modules::loadProgram(sourcePath, sources, diags, {moduleRoots});
    timer.mark("load");
    if (!astModule) {
        std::string msg = diags.render(sources);
        if (errOut) *errOut = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    std::optional<types::InferenceResult> inferred;
    if (infer) {
        inferred = types::inferModule(*astModule, diags);
        timer.mark("infer");
        if (diags.hasErrors() || !inferred) {
            std::string msg = diags.render(sources);
            if (errOut) *errOut = msg;
            else std::fputs(msg.c_str(), stderr);
            return 1;
        }
    }

    lower::InferStatsCollector statsCollector;
    auto ilModule = lower::lowerModule(*astModule, diags,
                                       inferred ? &*inferred : nullptr,
                                       hostGlobals.empty() ? nullptr : &hostGlobals,
                                       &sources,
                                       inferStats ? &statsCollector : nullptr);
    timer.mark("lower");
    if (diags.hasErrors() || !ilModule) {
        std::string msg = diags.render(sources);
        if (errOut) *errOut = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

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
        LLVMBackend objBackend;
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
    // is the same object emission followed by a different link. The only
    // compile-time difference between the two is where the runtime will be:
    // `setSharedRuntime` is what makes the object reach the runtime's data
    // symbols through import slots (llvm_abi.h) and marks the three names a
    // module publishes, and with it off this is byte for byte the object
    // bronze has always emitted.
    std::filesystem::path tempObj = uniqueTempObjPath(sourcePath);

    LLVMBackend backend;
    if (!entrySymbol.empty()) backend.setEntrySymbol(entrySymbol);
    backend.setHostGlobals(hostGlobals);
    backend.setSharedRuntime(emitShared);
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

    bool linked = emitShared ? linkSharedModule(tempObj.string(), outputPath, diags)
                             : linkExecutable(tempObj.string(), outputPath, diags);
    timer.mark("link");
    timer.total();

    std::error_code ec;
    if (std::filesystem::exists(tempObj, ec)) {
        std::filesystem::remove(tempObj, ec);
    }

    if (!linked) {
        std::string msg = diags.hasErrors() ? diags.render(sources) : "error: linking failed\n";
        if (errOut) *errOut = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    return 0;
#endif
}

int runDriver(int argc, char** argv) {
    if (argc < 2) return fail(kUsage);
    const std::string command = argv[1];

    if (command == "version") {
#if defined(NDEBUG)
        std::puts("bronze 0.1.0 (Release)");
#else
        std::puts("bronze 0.1.0 (Debug)");
#endif
        return 0;
    }

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
            } else if (sourcePath.empty()) {
                sourcePath = arg;
            } else {
                return fail("error: unexpected argument " + arg + "\n");
            }
        }
        if (sourcePath.empty()) return fail("error: missing <file>\n");
        return runTypes(sourcePath, nullptr, moduleRoots);
    }

    if (command == "il") {
        if (argc < 3) return fail("error: missing <file>\n");
        std::string sourcePath;
        std::string hostGlobalsPath;
        std::vector<modules::ModuleRoot> moduleRoots;
        bool infer = true;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--no-infer") {
                infer = false;
            } else if (arg == "--host-globals") {
                if (i + 1 < argc) {
                    hostGlobalsPath = argv[++i];
                } else {
                    return fail("error: missing argument for --host-globals\n");
                }
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
            } else if (sourcePath.empty()) {
                sourcePath = arg;
            } else {
                return fail("error: unexpected argument " + arg + "\n");
            }
        }
        if (sourcePath.empty()) return fail("error: missing <file>\n");
        return runIl(sourcePath, nullptr, infer, hostGlobalsPath, moduleRoots);
    }

    if (command == "build") {
        if (argc < 3) return fail("error: missing <file>\n");
        std::string sourcePath;
        std::string outputPath = "a.exe";
        std::string hostGlobalsPath;
        std::string entrySymbol;
        std::vector<modules::ModuleRoot> moduleRoots;
        bool infer = true;
        bool timings = false;
        bool emitObj = false;
        bool emitShared = false;
        bool inferStats = false;

        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--no-infer") {
                infer = false;
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
                        emitShared);
    }

    return fail(kUsage);
}

}  // namespace bronze::cli
