// Scripts compiled ahead of time through the evaluator (eval.h, "Ahead-of-time
// modules"): built to a module and its record, loaded, run, and refused once
// anything they were built from has changed.
//
// What each step asserts:
//
//  * THE SAME PROGRAM. The module is the IL compileScript would build for the
//    same text and options, so it keeps the evaluator's script semantics — the
//    trailing expression's value comes back as the result, and a top-level
//    `var` / function declaration becomes a global — and its module graph
//    (an `import` of a file beside the entry) is compiled in.
//
//  * A THROW IS A RESULT. A top level that throws hands the value back in the
//    CallResult, as runCompiledScript does; the process does not exit (embed's
//    runEntry would).
//
//  * THE RECORD IS A GATE. Editing an imported file, or compiling against
//    another host-global list, makes loadModule refuse with a reason and leave
//    the module unrun; a missing module file is refused the same way.
//
// Linked against the shared runtime ONLY (tests/shared_load says why): the
// module imports that library, and a process holding a static copy beside it
// would have two heaps. It lives in the runtime's directory so a loaded
// module's import resolves to the copy this process already mapped.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "embed/embed.h"
#include "eval/eval.h"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
namespace embed = bronze::embed;
namespace eval = bronze::eval;

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

void writeText(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

#ifdef _WIN32
constexpr const char* kModuleExt = ".dll";
#elif defined(__APPLE__)
constexpr const char* kModuleExt = ".dylib";
#else
constexpr const char* kModuleExt = ".so";
#endif

// The registry pinned empty (captured), as a fresh realm's is: after the first
// run this process's registry holds lib.js, and a compile reading it would be
// a different program (asserted below).
eval::EvalOptions optionsFor(const fs::path& dir) {
    eval::EvalOptions opts;
    opts.filename = (dir / "entry.js").generic_string();
    opts.entryResolvesAs = dir / "entry.js";
    opts.moduleRegistry = true;
    opts.externalModulesCaptured = true;
    return opts;
}

}  // namespace

int main() {
    embed::setupIo();
#ifdef _WIN32
    const long long pid = static_cast<long long>(::_getpid());
#else
    const long long pid = static_cast<long long>(::getpid());
#endif
    const fs::path dir = fs::temp_directory_path() / ("bronze-aot-module-" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    writeText(dir / "lib.js", "export function triple(x) { return x * 3; }\nexport const label = 'lib';\n");
    const std::string source =
        "import { triple, label } from './lib.js';\n"
        "var aotSeen = label + ':' + triple(14);\n"
        "function aotHelper() { return 'helper'; }\n"
        "aotSeen + '/' + aotHelper();\n";
    const std::string module = (dir / ("unit" + std::string(kModuleExt))).string();
    const std::string record = (dir / "unit.bzm").string();

    // ---- build -------------------------------------------------------------------
    const eval::ModuleBuildResult built = eval::buildModule(source, optionsFor(dir), module, record);
    check(built.success, "buildModule wrote the module and its record" +
                             (built.errorMessage.empty() ? std::string() : (": " + built.errorMessage)));
    check(fs::is_regular_file(module, ec) && fs::is_regular_file(record, ec), "both files are on disk");
    bool readLib = false;
    for (const auto& f : built.filesRead) readLib = readLib || f.find("lib.js") != std::string::npos;
    check(readLib, "the graph's files are reported (lib.js)");

    // ---- load and run ----------------------------------------------------------------
    {
        const eval::LoadedModule loaded = eval::loadModule(source, optionsFor(dir), record);
        check(loaded.ok, "loadModule accepts an unchanged program" + (loaded.ok ? std::string() : ": " + loaded.why));
        if (loaded.ok) {
            embed::ModuleHandle handle = 0;
            eval::EvalOptions runOpts = optionsFor(dir);
            runOpts.moduleHandleOut = &handle;
            embed::CallResult res = eval::runModule(loaded, runOpts);
            embed::Persistent value(res.value);
            check(!res.thrown, "the top level ran without throwing");
            check(embed::toUtf8(value.get()) == "lib:42/helper",
                  "the trailing expression's value is the result: " + embed::toUtf8(value.get()));
            embed::GlobalValue seen = embed::globalValue("aotSeen");
            check(seen.found && embed::toUtf8(seen.value) == "lib:42", "a top-level var is a global");
            check(handle != 0, "the run was bracketed as a module load");
        }
    }

    // ---- a throw comes back as a value -----------------------------------------------------
    {
        const fs::path tdir = dir / "throws";
        fs::create_directories(tdir, ec);
        const std::string throwing = "globalThis.beforeThrow = 1;\nthrow new Error('aot top level threw');\n";
        const std::string tmod = (tdir / ("unit" + std::string(kModuleExt))).string();
        const std::string trec = (tdir / "unit.bzm").string();
        const eval::ModuleBuildResult tb = eval::buildModule(throwing, optionsFor(tdir), tmod, trec);
        check(tb.success, "a throwing top level builds");
        const eval::LoadedModule tl = eval::loadModule(throwing, optionsFor(tdir), trec);
        check(tl.ok, "and loads");
        if (tl.ok) {
            embed::CallResult res = eval::runModule(tl, optionsFor(tdir));
            embed::Persistent value(res.value);
            check(res.thrown, "the throw is reported, the process lives");
            const std::string msg = embed::toUtf8(embed::getProperty(value.get(), "message"));
            check(msg == "aot top level threw", "with the thrown value: " + msg);
        }
    }

    // ---- what makes a record stale ---------------------------------------------------------
    {
        eval::EvalOptions other = optionsFor(dir);
        other.hostGlobals = {"someHostGlobal"};
        const eval::LoadedModule l = eval::loadModule(source, other, record);
        check(!l.ok && !l.why.empty(), "another host-global list is refused: " + l.why);
    }
    {
        const eval::LoadedModule l = eval::loadModule(source + "\n;1;\n", optionsFor(dir), record);
        check(!l.ok, "other entry text is refused: " + l.why);
    }
    {
        // The live registry: the run published lib.js, so a compile now binds
        // that instance instead of compiling the file in.
        eval::EvalOptions live = optionsFor(dir);
        live.externalModulesCaptured = false;
        const eval::LoadedModule l = eval::loadModule(source, live, record);
        check(!l.ok, "a realm whose registry has changed is refused: " + l.why);
    }
    writeText(dir / "lib.js", "export function triple(x) { return x * 4; }\nexport const label = 'lib';\n");
    {
        const eval::LoadedModule l = eval::loadModule(source, optionsFor(dir), record);
        check(!l.ok && l.why.find("lib.js") != std::string::npos, "an edited import is refused: " + l.why);
    }
    // Rebuilt from the edited source, it is current again; then its image goes.
    {
        const std::string module2 = (dir / ("unit2" + std::string(kModuleExt))).string();
        const eval::ModuleBuildResult b2 = eval::buildModule(source, optionsFor(dir), module2, record);
        check(b2.success, "a rebuild writes a fresh module under the same record");
        const eval::LoadedModule l = eval::loadModule(source, optionsFor(dir), record);
        check(l.ok, "the rebuilt module is current" + (l.ok ? std::string() : ": " + l.why));
        fs::path gone = dir / "gone.bzm";
        const std::string module3 = (dir / ("unit3" + std::string(kModuleExt))).string();
        eval::buildModule(source, optionsFor(dir), module3, gone.string());
        fs::remove(module3, ec);
        const eval::LoadedModule l3 = eval::loadModule(source, optionsFor(dir), gone.string());
        check(!l3.ok && l3.why.find("could not be loaded") != std::string::npos,
              "a missing module image is refused: " + l3.why);
    }

    fs::remove_all(dir, ec);  // best effort: Windows keeps the mapped images
    std::printf("%s\n", g_failures == 0 ? "aot-module: all passed" : "aot-module: FAILED");
    return g_failures == 0 ? 0 : 1;
}
