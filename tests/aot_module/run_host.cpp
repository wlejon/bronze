// `bronze_module_run_host <entry.js>`: the file built to an ahead-of-time
// module through the evaluator (eval::buildModule), loaded and run, as a host
// installing an app does. The oracle's module suite runs every case through
// it and holds the output to the pinned bytes: a module build is the one
// compile that runs the whole-program optimizer over the evaluator's script
// semantics (top-level declarations global, the trailing value kept).
//
// Linked against the shared runtime only, like bronze_aot_module_host.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
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

#ifdef _WIN32
constexpr const char* kModuleExt = ".dll";
#elif defined(__APPLE__)
constexpr const char* kModuleExt = ".dylib";
#else
constexpr const char* kModuleExt = ".so";
#endif

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: bronze_module_run_host <entry.js>\n");
        return 2;
    }
    embed::setupIo();
    const fs::path entry = fs::absolute(argv[1]);
    std::ifstream in(entry, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "error: cannot read %s\n", argv[1]);
        return 2;
    }
    std::stringstream text;
    text << in.rdbuf();
    const std::string source = text.str();

#ifdef _WIN32
    const long long pid = static_cast<long long>(::_getpid());
#else
    const long long pid = static_cast<long long>(::getpid());
#endif
    const fs::path dir = fs::temp_directory_path() / ("bronze-module-run-" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::string module = (dir / ("unit" + std::string(kModuleExt))).string();
    const std::string record = (dir / "unit.bzm").string();

    // The filename a `bronze run` of the case gives, so Error.stack and
    // import.meta.url read the same.
    eval::EvalOptions opts;
    opts.filename = argv[1];
    opts.entryResolvesAs = entry;

    int status = 0;
    {
        eval::installDefaultDynamicHooks();
        const eval::ModuleBuildResult built = eval::buildModule(source, opts, module, record);
        if (!built.success) {
            std::fprintf(stderr, "%s\n", built.errorMessage.c_str());
            status = 1;
        } else {
            const eval::LoadedModule loaded = eval::loadModule(source, opts, record);
            if (!loaded.ok) {
                std::fprintf(stderr, "error: the module just built was refused: %s\n", loaded.why.c_str());
                status = 1;
            } else {
                embed::CallResult res = eval::runModule(loaded, opts);
                if (res.thrown) {
                    const std::string report = embed::uncaughtReport(res.value);
                    std::fflush(stdout);
                    std::fprintf(stderr, "%s\n", report.c_str());
                    status = 1;
                }
            }
        }
        eval::stopBackgroundCompiles();
    }
    std::fflush(stdout);
    // The image stays mapped (its code may be on a stack until exit); its
    // files can go.
    fs::remove_all(dir, ec);
    return status;
}
