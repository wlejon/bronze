#include "cli/link.h"

#include <atomic>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <optional>
#include <string>
#include <vector>

#include "cli/link_order.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

namespace bronze::cli {
namespace {

std::filesystem::path getExecutableDir() {
#ifdef _WIN32
    char buffer[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, buffer, MAX_PATH);
    if (len > 0) {
        return std::filesystem::path(buffer).parent_path();
    }
#elif defined(__linux__)
    char buffer[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len > 0) {
        buffer[len] = '\0';
        return std::filesystem::path(buffer).parent_path();
    }
#elif defined(__APPLE__)
    char buffer[PATH_MAX];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) == 0) {
        char realBuffer[PATH_MAX];
        if (realpath(buffer, realBuffer) != nullptr) {
            return std::filesystem::path(realBuffer).parent_path();
        }
        return std::filesystem::path(buffer).parent_path();
    }
#endif
    return std::filesystem::current_path();
}

// `getenv` without the MSVC deprecation, in one place instead of at each of
// the two search functions that wants it.
std::optional<std::filesystem::path> envPath(const char* name) {
#ifdef _WIN32
    char* buffer = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buffer, &len, name) == 0 && buffer != nullptr) {
        std::filesystem::path p(buffer);
        std::free(buffer);
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return p;
    }
#else
    if (const char* value = std::getenv(name)) {
        std::filesystem::path p(value);
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return p;
    }
#endif
    return std::nullopt;
}

std::optional<std::filesystem::path> findRuntimeLib() {
    static std::optional<std::filesystem::path> s_cached;
    static std::once_flag s_once;
    std::call_once(s_once, [] {
        if (auto fromEnv = envPath("BRONZE_RT_LIB")) {
            s_cached = *fromEnv;
            return;
        }

        std::vector<std::filesystem::path> candidates;
        std::filesystem::path exeDir = getExecutableDir();

        const std::vector<const char*> libNames = {
#ifdef _WIN32
            "bronze_rt.lib", "libbronze_rt.a"
#else
            "libbronze_rt.a", "bronze_rt.lib"
#endif
        };

        for (const char* name : libNames) {
            candidates.push_back(exeDir / name);
            candidates.push_back(exeDir / "../rt" / name);
            candidates.push_back(exeDir / "../../rt" / name);
            candidates.push_back(exeDir / "../../../rt" / name);
            candidates.push_back(exeDir / "src/rt" / name);
            candidates.push_back(exeDir / "../src/rt" / name);
            candidates.push_back(exeDir / "../../src/rt" / name);
            candidates.push_back(exeDir / "../../../src/rt" / name);

            std::filesystem::path cwd = std::filesystem::current_path();
            candidates.push_back(cwd / name);
            candidates.push_back(cwd / "src/rt" / name);
            candidates.push_back(cwd / "build/dev/src/rt" / name);
            candidates.push_back(cwd / "build/src/rt" / name);
            candidates.push_back(cwd / "build/Release/src/rt" / name);
        }

        const size_t flatCount = candidates.size();
        static const char* const kConfigs[] = {"Release", "RelWithDebInfo", "MinSizeRel",
                                               "Debug"};
        for (size_t i = 0; i < flatCount; ++i) {
            for (const char* config : kConfigs) {
                candidates.push_back(candidates[i].parent_path() / config / candidates[i].filename());
            }
        }

        for (const auto& cand : candidates) {
            std::error_code ec;
            if (std::filesystem::exists(cand, ec)) {
                s_cached = std::filesystem::canonical(cand, ec);
                return;
            }
        }
    });
    return s_cached;
}

std::optional<std::filesystem::path> findRuntimeCpp() {
    static std::optional<std::filesystem::path> s_cached;
    static std::once_flag s_once;
    std::call_once(s_once, [] {
        std::vector<std::filesystem::path> candidates;
        std::filesystem::path exeDir = getExecutableDir();

        candidates.push_back(exeDir / "../../../src/rt/rt.cpp");
        candidates.push_back(exeDir / "../../src/rt/rt.cpp");
        candidates.push_back(exeDir / "../src/rt/rt.cpp");
        candidates.push_back(exeDir / "src/rt/rt.cpp");

        std::filesystem::path cwd = std::filesystem::current_path();
        candidates.push_back(cwd / "src/rt/rt.cpp");
        candidates.push_back(cwd / "../src/rt/rt.cpp");

        for (const auto& cand : candidates) {
            std::error_code ec;
            if (std::filesystem::exists(cand, ec)) {
                s_cached = std::filesystem::canonical(cand, ec);
                return;
            }
        }
    });
    return s_cached;
}

// The shared runtime, as the thing a MODULE links against: on Windows the
// import library beside the DLL, elsewhere the shared object itself. One name
// per platform and no cross-platform fallback list, because unlike the static
// search above there is no world in which the wrong one is better than none:
// linking a module against a static archive is the two-heaps failure this
// whole path exists to prevent, so a miss must stay a miss.
std::optional<std::filesystem::path> findSharedRuntime() {
    static std::optional<std::filesystem::path> s_cached;
    static std::once_flag s_once;
    std::call_once(s_once, [] {
        if (auto fromEnv = envPath("BRONZE_SHARED_RT_LIB")) {
            s_cached = *fromEnv;
            return;
        }

#ifdef _WIN32
        const char* const name = "bronze_runtime_shared.lib";
#elif defined(__APPLE__)
        const char* const name = "libbronze_runtime_shared.dylib";
#else
        const char* const name = "libbronze_runtime_shared.so";
#endif

        std::vector<std::filesystem::path> candidates;
        const std::filesystem::path exeDir = getExecutableDir();
        candidates.push_back(exeDir / name);
        candidates.push_back(exeDir / "shared" / name);
        candidates.push_back(exeDir / ".." / "shared" / name);
        candidates.push_back(exeDir / ".." / ".." / "shared" / name);

        const std::filesystem::path cwd = std::filesystem::current_path();
        candidates.push_back(cwd / name);
        candidates.push_back(cwd / "shared" / name);
        candidates.push_back(cwd / "build/dev/shared" / name);
        candidates.push_back(cwd / "build/shared" / name);

        // Multi-config generators (Visual Studio, Xcode) append a per-config
        // directory under the output dir CMake was given, so the library sits
        // one level deeper than every path above. The same directories again
        // with the four standard config names — after the flat ones, so a
        // single-config layout never changes its answer, and Release first
        // because a Debug runtime under a Release host is the CRT mismatch
        // embed.h forbids anyway.
        const size_t flatCount = candidates.size();
        static const char* const kConfigs[] = {"Release", "RelWithDebInfo", "MinSizeRel",
                                               "Debug"};
        for (size_t i = 0; i < flatCount; ++i) {
            for (const char* config : kConfigs) {
                candidates.push_back(candidates[i].parent_path() / config / name);
            }
        }

        for (const auto& cand : candidates) {
            std::error_code ec;
            if (std::filesystem::exists(cand, ec)) {
                s_cached = std::filesystem::canonical(cand, ec);
                return;
            }
        }
    });
    return s_cached;
}

std::atomic<uint64_t> g_tempCounter{0};

// A temp path unique per process and per call. `stem` names what it is for and
// `extension` includes its dot.
std::filesystem::path uniqueTempPath(const std::string& stem, const char* extension) {
    uint64_t pid = 0;
#ifdef _WIN32
    pid = static_cast<uint64_t>(GetCurrentProcessId());
#else
    pid = static_cast<uint64_t>(getpid());
#endif
    const uint64_t count = g_tempCounter.fetch_add(1, std::memory_order_relaxed);
    return std::filesystem::temp_directory_path() /
           (stem + "_" + std::to_string(pid) + "_" + std::to_string(count) + extension);
}

// Is the `exe` that PATH resolves actually the tool bronze means by that name?
//
// `link` is the collision that made this necessary, and it is not exotic: a Git
// for Windows install puts GNU coreutils' hardlink utility on PATH as
// `link.exe`, and cmd runs THAT for `link.exe /nologo /DLL ...`. Its complaint
// — "extra operand '/out:app.dll'", plus an invitation to try `link --help` —
// then surfaces as bronze's link failure, phrased by a program the user never
// meant to run about a flag they never typed.
//
// So a tool whose name is not its own is IDENTIFIED before it is trusted: run
// it with a harmless flag, capture what it says about itself, and require the
// vendor's own words. Probed at most once per name per process, and only when
// the fallback chain actually reaches that candidate — the first linker
// normally works, so the happy path never pays for this.
bool toolIsItself(const std::string& exe, const std::string& probeArgs,
                  const std::string& needle) {
    static std::mutex s_mutex;
    static std::map<std::string, bool> s_cache;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_cache.find(exe);
        if (it != s_cache.end()) return it->second;
    }

    const std::filesystem::path out = uniqueTempPath("bronze_tool_probe", ".txt");
    const std::string command = exe + " " + probeArgs + " > \"" + out.string() + "\" 2>&1";
    std::system(command.c_str());

    bool identified = false;
    {
        std::ifstream in(out, std::ios::binary);
        if (in) {
            std::ostringstream text;
            text << in.rdbuf();
            identified = text.str().find(needle) != std::string::npos;
        }
    }
    std::error_code ec;
    std::filesystem::remove(out, ec);

    std::lock_guard<std::mutex> lock(s_mutex);
    s_cache[exe] = identified;
    return identified;
}

// The MSVC linker, as opposed to whatever else on this machine answers to
// `link`. `/?` makes the real one print "Microsoft (R) Incremental Linker
// Version ..." and makes coreutils' one complain about a missing operand.
bool msvcLinkIsAvailable() {
    return toolIsItself("link.exe", "/?", "Microsoft (R) Incremental Linker");
}

struct LinkerState {
    int workingIndex = -1;
    std::string libStr;
    std::string runtimeLibStr;
    std::string runtimeWholeStr;
    std::string unixRuntimeLibs;
    std::string cppStr;
    std::mutex mutex;
};

// The program a command line runs, for a diagnostic that has to say what was
// attempted. The first token, which is all these commands ever put there.
std::string commandTool(const std::string& command) {
    const auto end = command.find(' ');
    return end == std::string::npos ? command : command.substr(0, end);
}

// Runs `makeCommand(i)` over the platform's candidate list, cached-first, and
// answers whether one produced `outputPath`. The cache is the reason a build
// that compiles hundreds of programs does not pay for the misses twice.
//
// `triedOut` collects the tools actually RUN, so a total failure can name them.
// A candidate `makeCommand` declined (an empty string: the toolchain is absent,
// or the name on PATH turned out to be something else) is not among them —
// naming a tool bronze never launched would send the reader after the wrong
// thing.
bool runFirstWorkingCommand(LinkerState& state, int totalCommands,
                            const std::function<std::string(int)>& makeCommand,
                            const std::string& outputPath, std::string& triedOut) {
    auto note = [&triedOut](const std::string& cmd) {
        const std::string tool = commandTool(cmd);
        if (triedOut.find(tool) == std::string::npos) {
            triedOut += (triedOut.empty() ? "" : ", ") + tool;
        }
    };

    int cachedIndex = -1;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        cachedIndex = state.workingIndex;
    }

    if (cachedIndex >= 0) {
        std::string cmd = makeCommand(cachedIndex);
        note(cmd);
        int res = std::system(cmd.c_str());
        std::error_code ec;
        if (res == 0 && std::filesystem::exists(outputPath, ec)) {
            return true;
        }
    }

    for (int i = 0; i < totalCommands; ++i) {
        if (i == cachedIndex) continue;
        std::string cmd = makeCommand(i);
        if (cmd.empty()) continue;
        note(cmd);
        int res = std::system(cmd.c_str());
        std::error_code ec;
        if (res == 0 && std::filesystem::exists(outputPath, ec)) {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.workingIndex = i;
            return true;
        }
    }
    return false;
}

}  // namespace

bool linkExecutable(const std::vector<std::string>& objPaths, const std::string& outputPath,
                    DiagnosticSink& diags) {
    // Every command below wraps `objPath` in exactly one pair of quotes, so
    // joining the list with `" "` splices N quoted paths into that pair and
    // no command line needs to know how many objects there are.
    //
    // The order is `orderForLink`'s rather than the caller's because the
    // linker lays the image out in the order it is given the objects, and that
    // layout is a measurable variable the harness needs to be able to move
    // (link_order.h). With no seed set it IS the caller's order, unchanged.
    const std::vector<std::string> ordered = orderForLink(objPaths);
    std::string objPath;
    for (size_t i = 0; i < ordered.size(); ++i) {
        if (i) objPath += "\" \"";
        objPath += ordered[i];
    }
    auto rtLib = findRuntimeLib();
    auto rtCpp = findRuntimeCpp();

    if (!rtLib && !rtCpp) {
        diags.error(Span{}, "Runtime library (libbronze_rt.a/bronze_rt.lib) or runtime source (rt.cpp) not found");
        return false;
    }

    static LinkerState s_state;
    static std::once_flag s_stringsOnce;
    std::call_once(s_stringsOnce, [&] {
        if (rtLib) {
            s_state.libStr = rtLib->string();
            static const char* const kRuntimeLibs[][3] = {
                {"runtime", "libbronze_runtime.a", "bronze_runtime.lib"},
                {"json", "libbronze_json.a", "bronze_json.lib"},
                {"regex", "libbronze_regex.a", "bronze_regex.lib"},
                {"support", "libbronze_support.a", "bronze_support.lib"},
            };
            for (const auto& lib : kRuntimeLibs) {
                std::filesystem::path path;
                for (int nameIdx = 1; nameIdx <= 2; ++nameIdx) {
                    std::vector<std::filesystem::path> libCandidates = {
                        rtLib->parent_path() / lib[nameIdx],
                        rtLib->parent_path() / ".." / lib[0] / lib[nameIdx],
                        rtLib->parent_path() / ".." / ".." / lib[0] / rtLib->parent_path().filename() / lib[nameIdx],
                        rtLib->parent_path() / ".." / ".." / lib[0] / lib[nameIdx],
                        rtLib->parent_path() / ".." / ".." / "src" / lib[0] / rtLib->parent_path().filename() / lib[nameIdx],
                        rtLib->parent_path() / ".." / ".." / "src" / lib[0] / lib[nameIdx],
                    };
                    for (const auto& cand : libCandidates) {
                        std::error_code ec;
                        if (std::filesystem::exists(cand, ec)) {
                            path = std::filesystem::canonical(cand, ec);
                            break;
                        }
                    }
                    if (!path.empty()) break;
                }
                if (path.empty()) continue;
                const std::string quoted = "\"" + path.string() + "\"";
                s_state.runtimeLibStr += (s_state.runtimeLibStr.empty() ? "" : " ") + quoted;
                s_state.runtimeWholeStr += (s_state.runtimeWholeStr.empty() ? "" : " ") + ("/wholearchive:" + quoted);
                s_state.unixRuntimeLibs += (s_state.unixRuntimeLibs.empty() ? "" : " ") + quoted;
            }
        }
        if (rtCpp) {
            s_state.cppStr = rtCpp->string();
        }
    });

    auto makeCommand = [&](int index) -> std::string {
#ifdef _WIN32
        // /DEBUG /OPT:REF /OPT:ICF for the same reason the --emit-shared path
        // carries them (the long comment there): the COFF objects carry a
        // symbol per compiled JS function, /DEBUG copies those into a PDB
        // beside the exe — which is what lets BRONZE_SAMPLE name a frame
        // `hotPath` instead of `program.exe+0x1a4c` — and the two /OPT
        // switches undo the fold defaults /DEBUG silently flips, so the
        // binary stays byte-for-byte what it was without them.
        switch (index) {
            case 0:
                return "lld-link /nologo /subsystem:console /include:main /DEBUG /OPT:REF /OPT:ICF /out:\"" + outputPath + "\" \"" + objPath + "\" \"" + s_state.libStr + "\" " + s_state.runtimeLibStr;
            case 1:
                return "lld-link /nologo /subsystem:console /DEBUG /OPT:REF /OPT:ICF /wholearchive:\"" + s_state.libStr + "\" " + s_state.runtimeWholeStr + " /out:\"" + outputPath + "\" \"" + objPath + "\"";
            case 2:
                if (!msvcLinkIsAvailable()) return "";
                return "link.exe /nologo /subsystem:console /include:main /DEBUG /OPT:REF /OPT:ICF /out:\"" + outputPath + "\" \"" + objPath + "\" \"" + s_state.libStr + "\" " + s_state.runtimeLibStr;
            case 3:
                if (!msvcLinkIsAvailable()) return "";
                return "link.exe /nologo /subsystem:console /DEBUG /OPT:REF /OPT:ICF /wholearchive:\"" + s_state.libStr + "\" " + s_state.runtimeWholeStr + " /out:\"" + outputPath + "\" \"" + objPath + "\"";
            case 4:
                return "clang-cl /nologo \"" + objPath + "\" \"" + s_state.libStr + "\" " + s_state.runtimeLibStr + " /link /include:main /Fe:\"" + outputPath + "\"";
            case 5:
                return "cl.exe /nologo \"" + objPath + "\" \"" + s_state.libStr + "\" " + s_state.runtimeLibStr + " /link /include:main /Fe:\"" + outputPath + "\"";
            case 6:
                return "clang++ \"" + objPath + "\" -Wl,--whole-archive \"" + s_state.libStr + "\" " + s_state.unixRuntimeLibs + " -Wl,--no-whole-archive -pthread -ldl -lm -o \"" + outputPath + "\"";
            case 7:
                return "g++ \"" + objPath + "\" -Wl,--whole-archive \"" + s_state.libStr + "\" " + s_state.unixRuntimeLibs + " -Wl,--no-whole-archive -pthread -ldl -lm -o \"" + outputPath + "\"";
            case 8:
                return "clang-cl /nologo /std:c++20 \"" + objPath + "\" \"" + s_state.cppStr + "\" /Fe:\"" + outputPath + "\"";
            case 9:
                return "cl.exe /nologo /std:c++20 \"" + objPath + "\" \"" + s_state.cppStr + "\" /Fe:\"" + outputPath + "\"";
            case 10:
                return "clang++ -std=c++20 \"" + objPath + "\" \"" + s_state.cppStr + "\" -pthread -ldl -lm -o \"" + outputPath + "\"";
            case 11:
                return "g++ -std=c++20 \"" + objPath + "\" \"" + s_state.cppStr + "\" -pthread -ldl -lm -o \"" + outputPath + "\"";
            default:
                return "";
        }
#elif defined(__APPLE__)
        switch (index) {
            case 0:
                return "clang++ -w \"" + objPath + "\" -Wl,-force_load,\"" + s_state.libStr + "\" " + s_state.unixRuntimeLibs + " -o \"" + outputPath + "\"";
            case 1:
                return "clang++ -w \"" + objPath + "\" \"" + s_state.libStr + "\" " + s_state.unixRuntimeLibs + " -o \"" + outputPath + "\"";
            case 2:
                return "clang++ -w -std=c++20 \"" + objPath + "\" \"" + s_state.cppStr + "\" -o \"" + outputPath + "\"";
            case 3:
                return "g++ -w \"" + objPath + "\" \"" + s_state.libStr + "\" " + s_state.unixRuntimeLibs + " -o \"" + outputPath + "\"";
            case 4:
                return "g++ -w -std=c++20 \"" + objPath + "\" \"" + s_state.cppStr + "\" -o \"" + outputPath + "\"";
            case 5:
                return "clang++ -w -Wl,-all_load \"" + objPath + "\" \"" + s_state.libStr + "\" " + s_state.unixRuntimeLibs + " -o \"" + outputPath + "\"";
            default:
                return "";
        }
#else
        switch (index) {
            case 0:
                return "clang++ -no-pie \"" + objPath + "\" -Wl,--whole-archive \"" + s_state.libStr + "\" " + s_state.unixRuntimeLibs + " -Wl,--no-whole-archive -pthread -ldl -lm -o \"" + outputPath + "\"";
            case 1:
                return "g++ -no-pie \"" + objPath + "\" -Wl,--whole-archive \"" + s_state.libStr + "\" " + s_state.unixRuntimeLibs + " -Wl,--no-whole-archive -pthread -ldl -lm -o \"" + outputPath + "\"";
            case 2:
                return "clang++ -no-pie \"" + objPath + "\" \"" + s_state.libStr + "\" " + s_state.unixRuntimeLibs + " -pthread -ldl -lm -o \"" + outputPath + "\"";
            case 3:
                return "g++ -no-pie \"" + objPath + "\" \"" + s_state.libStr + "\" " + s_state.unixRuntimeLibs + " -pthread -ldl -lm -o \"" + outputPath + "\"";
            case 4:
                return "clang++ -no-pie -std=c++20 \"" + objPath + "\" \"" + s_state.cppStr + "\" -pthread -ldl -lm -o \"" + outputPath + "\"";
            case 5:
                return "g++ -no-pie -std=c++20 \"" + objPath + "\" \"" + s_state.cppStr + "\" -pthread -ldl -lm -o \"" + outputPath + "\"";
            case 6:
                return "lld-link /nologo /subsystem:console /include:main /out:\"" + outputPath + "\" \"" + objPath + "\" \"" + s_state.libStr + "\" " + s_state.runtimeLibStr;
            case 7:
                if (!msvcLinkIsAvailable()) return "";
                return "link.exe /nologo /subsystem:console /include:main /out:\"" + outputPath + "\" \"" + objPath + "\" \"" + s_state.libStr + "\" " + s_state.runtimeLibStr;
            default:
                return "";
        }
#endif
    };

    std::string tried;
    if (runFirstWorkingCommand(s_state, 12, makeCommand, outputPath, tried)) return true;

    diags.error(Span{}, "Failed to link an executable. Tried: " +
                            (tried.empty() ? std::string("nothing — no linker bronze knows was "
                                                         "found on PATH")
                                           : tried) +
                            ". bronze needs a system linker: on Windows lld-link, or MSVC's "
                            "link.exe from a developer prompt; elsewhere clang++ or g++.");
    return false;
}

bool linkSharedModule(const std::vector<std::string>& objPaths, const std::string& outputPath,
                      DiagnosticSink& diags) {
    // Quote-splice join, exactly as linkExecutable does it.
    std::string objPath;
    for (size_t i = 0; i < objPaths.size(); ++i) {
        if (i) objPath += "\" \"";
        objPath += objPaths[i];
    }
    auto sharedRt = findSharedRuntime();
    if (!sharedRt) {
        diags.error(Span{},
                    "--emit-shared: the shared bronze runtime was not found "
#ifdef _WIN32
                    "(bronze_runtime_shared.lib, the import library beside "
                    "bronze_runtime_shared.dll). "
#elif defined(__APPLE__)
                    "(libbronze_runtime_shared.dylib). "
#else
                    "(libbronze_runtime_shared.so). "
#endif
                    "Point BRONZE_SHARED_RT_LIB at it, or build bronze with "
                    "-DBRONZE_BUILD_SHARED_RUNTIME=ON. A loadable module is NOT linked "
                    "against the static runtime as a fallback: two runtimes in one "
                    "process means two heaps.");
        return false;
    }

    static LinkerState s_state;
    static std::once_flag s_stringsOnce;
    std::call_once(s_stringsOnce, [&] {
        s_state.libStr = sharedRt->string();
        // Where the loader has to find the runtime at run time, which on the
        // two rpath platforms is a link-time fact about the module.
        s_state.runtimeLibStr = sharedRt->parent_path().string();
    });

    auto makeCommand = [&](int index) -> std::string {
#ifdef _WIN32
        // The module needs two things from the C runtime that its own object
        // cannot supply: `_fltused`, the tag MSVC's linker demands of anything
        // that touches floating point, and the default DllMain that gives a DLL
        // an entry point. msvcrt is the IMPORT library for the SHARED CRT, so
        // asking for it adds no second C runtime to the process — that is the
        // whole difference between msvcrt and libcmt. Module and runtime DLL
        // bind the same ucrtbase/vcruntime, and nothing crosses between them
        // anyway: the module's entire surface is the C ABI, u64 in and u64 out,
        // which owns no CRT object.
        //
        // /DEFAULTLIB rather than naming `msvcrt.lib` as an input, and the
        // difference is the whole of whether this works outside a developer
        // prompt. An input file is a PATH the linker opens, searched only along
        // %LIB% and /libpath; a defaultlib request goes through lld-link's own
        // MSVC and Windows SDK detection, which needs no environment at all.
        // That is also exactly how the STATIC path gets its env-independence —
        // bronze_rt.lib's MSVC-compiled objects carry /DEFAULTLIB: directives
        // in their .drectve sections, and lld-link resolves those the same way.
        // A bronze-emitted object carries no directives, so the request has to
        // be made on the command line; making it as an input file is what broke
        // `--emit-shared` from a shell with an empty %LIB%.
        // /DEBUG, and the two /OPT switches that undo what it changes.
        //
        // A module is a DLL full of anonymous machine code otherwise: a sampling
        // profiler, a crash dump and a debugger can all name the MODULE a stack
        // frame is in and nothing finer, because the compiled functions have
        // internal linkage and are not exported. The COFF objects bronze emits
        // DO carry a symbol per function, and /DEBUG is what asks the linker to
        // copy those into a PDB beside the module — which is the whole of what
        // `brobench/analysis/chunk4_native_bill.md` needed to attribute a frame
        // to a JS function rather than to "app.dll+0x1a4c20".
        //
        // The two /OPT switches are not an optimisation request; they are what
        // keeps the module BYTE-FOR-BYTE what it was. /DEBUG silently flips the
        // defaults to /OPT:NOREF /OPT:NOICF, so without them a module built for
        // measurement would not be the module that ships, and the measurement
        // would be of something else.
        //
        // ELF and Mach-O need no equivalent: a .so and a .dylib carry a symbol
        // table with local function symbols already, and perf and Instruments
        // read it.
        switch (index) {
            case 0:
                return "lld-link /nologo /DLL /DEBUG /OPT:REF /OPT:ICF /defaultlib:msvcrt /out:\"" +
                       outputPath + "\" \"" + objPath + "\" \"" + s_state.libStr + "\"";
            case 1:
                // MSVC's own linker has no such detection and does need %LIB%,
                // which is why it is second — and it is identified before it is
                // run, because `link` is not a name MSVC has to itself.
                if (!msvcLinkIsAvailable()) return "";
                return "link.exe /nologo /DLL /DEBUG /OPT:REF /OPT:ICF /defaultlib:msvcrt /out:\"" +
                       outputPath + "\" \"" + objPath + "\" \"" + s_state.libStr + "\"";
            default:
                return "";
        }
#elif defined(__APPLE__)
        switch (index) {
            case 0:
                return "clang++ -w -dynamiclib \"" + objPath + "\" \"" + s_state.libStr +
                       "\" -Wl,-rpath,\"" + s_state.runtimeLibStr + "\" -o \"" + outputPath + "\"";
            case 1:
                return "g++ -w -dynamiclib \"" + objPath + "\" \"" + s_state.libStr +
                       "\" -Wl,-rpath,\"" + s_state.runtimeLibStr + "\" -o \"" + outputPath + "\"";
            default:
                return "";
        }
#else
        switch (index) {
            case 0:
                return "clang++ -shared \"" + objPath + "\" \"" + s_state.libStr +
                       "\" -Wl,-rpath,\"" + s_state.runtimeLibStr + "\" -o \"" + outputPath + "\"";
            case 1:
                return "g++ -shared \"" + objPath + "\" \"" + s_state.libStr + "\" -Wl,-rpath,\"" +
                       s_state.runtimeLibStr + "\" -o \"" + outputPath + "\"";
            default:
                return "";
        }
#endif
    };

    std::string tried;
    if (runFirstWorkingCommand(s_state, 2, makeCommand, outputPath, tried)) return true;

    diags.error(Span{},
                "--emit-shared: failed to link the module against " + s_state.libStr +
                    ". Tried: " +
                    (tried.empty() ? std::string("nothing — no linker bronze knows was found "
                                                 "on PATH")
                                   : tried) +
#ifdef _WIN32
                    ". bronze needs lld-link on PATH, or MSVC's link.exe with %LIB% set (a "
                    "developer prompt). Point BRONZE_SHARED_RT_LIB at the runtime's import "
                    "library if it is not the one named above."
#else
                    ". bronze needs clang++ or g++ on PATH. Point BRONZE_SHARED_RT_LIB at the "
                    "shared runtime if it is not the one named above."
#endif
    );
    return false;
}

std::filesystem::path uniqueTempObjPath(const std::string& sourcePath) {
    const std::string stem = std::filesystem::path(sourcePath).stem().string() + "_temp";
#ifdef _WIN32
    return uniqueTempPath(stem, ".obj");
#else
    return uniqueTempPath(stem, ".o");
#endif
}

}  // namespace bronze::cli
