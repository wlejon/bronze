#include "cli/link.h"

#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

#include <brass/target/aot_linker.hpp>

#include "abi/bronze_abi.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bronze::cli {
namespace {

namespace fs = std::filesystem;

// ---- names -----------------------------------------------------------------

// The shared runtime's file name beside this bronze, and the prebuilt host
// next to it: files of THIS machine.
#ifdef _WIN32
constexpr const char* kRuntimeFile = "bronze_runtime_shared.dll";
constexpr const char* kHostFile = "bronze_host.exe";
#elif defined(__APPLE__)
constexpr const char* kRuntimeFile = "libbronze_runtime_shared.dylib";
constexpr const char* kHostFile = "bronze_host";
#else
constexpr const char* kRuntimeFile = "libbronze_runtime_shared.so";
constexpr const char* kHostFile = "bronze_host";
#endif

// What a module imports the runtime as — the DLL name on Windows, the soname
// on ELF, and on Mach-O the install name the runtime was built with, which
// CMake spells `@rpath/<file>` — plus the module's own extension and the
// search path for its own directory. Spelled for the loader of the machine
// the OBJECT is for, which is this one unless `--target` said otherwise.
//
// The C math library is the one other thing generated code calls: brass
// lowers Math.sqrt, Math.floor and their kin to the C functions of those
// names, and a statically linked program took them from the C runtime it
// was linked with. A module imports them from the platform's own library —
// the UCRT's math API set on Windows, libm on ELF, libSystem on Mach-O.
struct TargetNames {
    const char* runtimeImport;
    const char* mathImport;
    const char* moduleExtension;
    const char* ownDirRpath;   // none on Windows, where the loader looks beside the host
};

TargetNames namesFor(const brass::Target& target) {
    if (target.is_windows()) {
        return {"bronze_runtime_shared.dll", "api-ms-win-crt-math-l1-1-0.dll", ".dll", nullptr};
    }
    if (target.is_macos()) {
        return {"@rpath/libbronze_runtime_shared.dylib", "/usr/lib/libSystem.B.dylib", ".dylib",
                "@loader_path"};
    }
    return {"libbronze_runtime_shared.so", "libm.so.6", ".so", "$ORIGIN"};
}

// ---- where the runtime lives -------------------------------------------------

fs::path getExecutableDir() {
#ifdef _WIN32
    char buffer[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, buffer, MAX_PATH);
    if (len > 0) {
        return fs::path(buffer).parent_path();
    }
#elif defined(__linux__)
    char buffer[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len > 0) {
        buffer[len] = '\0';
        return fs::path(buffer).parent_path();
    }
#elif defined(__APPLE__)
    char buffer[PATH_MAX];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) == 0) {
        char realBuffer[PATH_MAX];
        if (realpath(buffer, realBuffer) != nullptr) {
            return fs::path(realBuffer).parent_path();
        }
        return fs::path(buffer).parent_path();
    }
#endif
    return fs::current_path();
}

// `getenv` without the MSVC deprecation.
std::optional<fs::path> envPath(const char* name) {
#ifdef _WIN32
    char* buffer = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buffer, &len, name) == 0 && buffer != nullptr) {
        fs::path p(buffer);
        std::free(buffer);
        std::error_code ec;
        if (fs::exists(p, ec)) return p;
    }
#else
    if (const char* value = std::getenv(name)) {
        fs::path p(value);
        std::error_code ec;
        if (fs::exists(p, ec)) return p;
    }
#endif
    return std::nullopt;
}

// The directory holding the shared runtime — and with it the prebuilt host,
// since src/host lands both in one place (BRONZE_SHARED_RUNTIME_DIR).
//
// BRONZE_SHARED_RT_LIB still names the runtime first, as it did when it named
// the import library a linker needed: a build that knows exactly which
// runtime its module must bind to says so, and a search that found a stale
// one elsewhere would be a silently different build. Whether it points at
// the library or its import stub, the DIRECTORY is what matters now.
//
// Then the places a bronze binary keeps its runtime: beside itself (a
// packaged install is flat), the `shared` output directory of the build tree
// it was built in, and the same directories under the multi-config
// generators' per-config names.
std::optional<fs::path> findSharedRuntimeDir() {
    static std::optional<fs::path> s_cached;
    static std::once_flag s_once;
    std::call_once(s_once, [] {
        if (auto fromEnv = envPath("BRONZE_SHARED_RT_LIB")) {
            std::error_code ec;
            const fs::path dir = fs::canonical(*fromEnv, ec).parent_path();
            if (!ec) {
                s_cached = dir;
                return;
            }
        }

        std::vector<fs::path> candidates;
        const fs::path exeDir = getExecutableDir();
        candidates.push_back(exeDir);
        candidates.push_back(exeDir / "shared");
        candidates.push_back(exeDir / ".." / "shared");
        candidates.push_back(exeDir / ".." / ".." / "shared");
        // A multi-config generator puts the exe one level deeper, in its
        // config directory (build/src/cli/Release), and the runtime in the
        // matching one under shared/ (build/shared/Release) — the same config,
        // so a Debug bronze never stages a Release runtime.
        candidates.push_back(exeDir / ".." / ".." / ".." / "shared" / exeDir.filename());

        const fs::path cwd = fs::current_path();
        candidates.push_back(cwd);
        candidates.push_back(cwd / "shared");
        candidates.push_back(cwd / "build/dev/shared");
        candidates.push_back(cwd / "build/shared");

        const size_t flatCount = candidates.size();
        static const char* const kConfigs[] = {"Release", "RelWithDebInfo", "MinSizeRel",
                                               "Debug"};
        for (size_t i = 0; i < flatCount; ++i) {
            for (const char* config : kConfigs) {
                candidates.push_back(candidates[i] / config);
            }
        }

        for (const auto& dir : candidates) {
            std::error_code ec;
            if (fs::exists(dir / kRuntimeFile, ec)) {
                s_cached = fs::canonical(dir, ec);
                return;
            }
        }
    });
    return s_cached;
}

std::string runtimeNotFoundMessage() {
    return std::string("the shared bronze runtime (") + kRuntimeFile +
           ") was not found beside bronze or in its build tree's shared/ directory. Point "
           "BRONZE_SHARED_RT_LIB at it, or build bronze with -DBRONZE_BUILD_SHARED_RUNTIME=ON.";
}

// ---- what the runtime exports -----------------------------------------------

// The export surface of the shared runtime, as the object sees it: the ABI
// registry, expanded here the same way cmake/bronze_abi_exports.cmake expands
// it into the .def / version script / exported-symbols list, plus the three
// brass words that script appends. One registry, two consumers, no drift.
const std::unordered_set<std::string>& runtimeExports() {
    static const std::unordered_set<std::string> s_names = [] {
        std::unordered_set<std::string> names;
#define BRONZE_LINK_EXPORT_NAME(name, ret, args) names.insert(#name);
        BRONZE_ABI_FUNCTIONS(BRONZE_LINK_EXPORT_NAME)
#undef BRONZE_LINK_EXPORT_NAME
        names.insert("brass_tlab_top");
        names.insert("brass_tlab_end");
        names.insert("brass_root_shape");
        return names;
    }();
    return s_names;
}

// The C math functions the backend may name (brass's il_lowering_ops and the
// kernel builders), every one of them a `double(double)` or `double(double,
// double)` the platform's math library exports under exactly that name.
const std::unordered_set<std::string>& mathExports() {
    static const std::unordered_set<std::string> s_names = {
        "acos", "asin", "atan", "atan2", "cbrt", "ceil", "cos", "cosh", "exp", "exp2",
        "expm1", "fabs", "floor", "fmax", "fmin", "fmod", "hypot", "log", "log10", "log1p",
        "log2", "pow", "round", "sin", "sinh", "sqrt", "tan", "tanh", "trunc",
    };
    return s_names;
}

// Every undefined symbol a relocation in the object names, once each, in
// first-reference order. A symbol the object declares undefined but never
// references is nobody's business.
std::vector<std::string> referencedUndefined(const brass::object::ObjectFile& obj) {
    std::unordered_set<std::string> seen;
    std::vector<std::string> names;
    for (const auto& sec : obj.sections) {
        for (const auto& r : sec.relocations) {
            if (r.symbol_name.empty() || seen.count(r.symbol_name)) continue;
            const auto* sym = obj.find_symbol(r.symbol_name);
            if (sym && sym->section_index >= 0) continue;
            if (!sym && obj.get_section(r.symbol_name) != nullptr) continue;
            seen.insert(r.symbol_name);
            names.push_back(r.symbol_name);
        }
    }
    return names;
}

// The exported names of the loadable-module contract (bronze_abi.h) for an
// entry, plus the code-range pair, kept to the ones the object defines.
std::vector<std::string> moduleExports(const brass::object::ObjectFile& obj,
                                       const std::string& entry) {
    const bool defaultEntry = entry == "bronze_main";
    const std::string prefix = defaultEntry ? "bronze_object" : entry;
    const std::vector<std::string> wanted = {
        entry,
        prefix + "_abi_fingerprint",
        entry + "_host_globals",
        entry + "_native_imports",
        prefix + "_code_ranges",
        prefix + "_code_range_count",
    };
    std::vector<std::string> present;
    for (const std::string& name : wanted) {
        const auto* sym = obj.find_symbol(name);
        if (sym && sym->section_index >= 0) present.push_back(name);
    }
    return present;
}

// ---- staging the runtime beside a program -------------------------------------

// Is `dst` a copy of `src` that is at least as new? Size and time, which is
// what a rebuilt runtime changes; the ABI stamp inside catches the drift a
// same-sized rebuild would hide.
bool stagedCopyIsCurrent(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    if (!fs::exists(dst, ec)) return false;
    if (fs::file_size(dst, ec) != fs::file_size(src, ec) || ec) return false;
    const auto srcTime = fs::last_write_time(src, ec);
    if (ec) return false;
    const auto dstTime = fs::last_write_time(dst, ec);
    if (ec) return false;
    return dstTime >= srcTime;
}

// A file copy whose descriptors no other thread's child can inherit. This
// matters because a build may run in-process beside threads that fork and
// exec (the oracle suite compiles its cases on N threads and runs each one
// through popen): a write descriptor that is open across another thread's
// fork lives on in that child for as long as it runs, and the kernel refuses
// to exec a file anyone still holds open for writing — ETXTBSY, "Text file
// busy", on a program that was written completely a moment ago. Descriptors
// opened O_CLOEXEC close at the child's exec, so nothing outlives the copy.
// std::filesystem::copy_file gives no such guarantee; Windows does not
// inherit handles into a child unless asked to, so the library call stands
// there.
bool copyFileForExec(const fs::path& src, const fs::path& dst, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    return fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec) && !ec;
#else
    const int in = ::open(src.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        ec = std::error_code(errno, std::generic_category());
        return false;
    }
    struct stat st{};
    if (::fstat(in, &st) != 0) {
        ec = std::error_code(errno, std::generic_category());
        ::close(in);
        return false;
    }
    const int out = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 07777);
    if (out < 0) {
        ec = std::error_code(errno, std::generic_category());
        ::close(in);
        return false;
    }
    std::vector<char> buffer(1 << 16);
    bool ok = true;
    for (;;) {
        const ssize_t got = ::read(in, buffer.data(), buffer.size());
        if (got == 0) break;
        if (got < 0) {
            if (errno == EINTR) continue;
            ec = std::error_code(errno, std::generic_category());
            ok = false;
            break;
        }
        ssize_t done = 0;
        while (done < got) {
            const ssize_t put = ::write(out, buffer.data() + done, static_cast<size_t>(got - done));
            if (put < 0) {
                if (errno == EINTR) continue;
                ec = std::error_code(errno, std::generic_category());
                ok = false;
                break;
            }
            done += put;
        }
        if (!ok) break;
    }
    // The mode again, past the umask the open applied.
    if (ok && ::fchmod(out, st.st_mode & 07777) != 0) {
        ec = std::error_code(errno, std::generic_category());
        ok = false;
    }
    if (::close(out) != 0 && ok) {
        ec = std::error_code(errno, std::generic_category());
        ok = false;
    }
    ::close(in);
    return ok;
#endif
}

// Copy a library the program loads beside it, unless the copy there is
// already current. Written to a unique name and renamed into place, because
// N builds may stage into one directory at once (the oracle suite compiles
// its cases in parallel, into one temp directory): a half-written library
// is never at the final name, and a loser of the rename race finds the
// winner's copy already there. `what` names the library in a diagnostic.
bool stageBeside(const fs::path& src, const fs::path& outDir, const char* what,
                 DiagnosticSink& diags) {
    static std::mutex s_mutex;
    std::lock_guard<std::mutex> lock(s_mutex);

    const fs::path dst = outDir / src.filename();
    std::error_code ec;
    if (fs::equivalent(src, dst, ec)) return true;
    if (stagedCopyIsCurrent(src, dst)) return true;

    uint64_t pid = 0;
#ifdef _WIN32
    pid = static_cast<uint64_t>(GetCurrentProcessId());
#else
    pid = static_cast<uint64_t>(getpid());
#endif
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path partial = outDir / (src.filename().string() + "." + std::to_string(pid) + "." +
                                       std::to_string(stamp) + ".partial");
    if (!copyFileForExec(src, partial, ec)) {
        const std::string why = ec.message();
        fs::remove(partial, ec);
        diags.error(Span{}, std::string("cannot copy ") + what + " " + src.string() + " to " +
                                dst.string() + ": " + why);
        return false;
    }
    fs::rename(partial, dst, ec);
    if (ec) {
        // The destination may be in use by a running program, or another
        // build may have just put the same library there. Either way a
        // current copy at the final name is the outcome that was wanted.
        std::error_code cleanup;
        fs::remove(partial, cleanup);
        if (stagedCopyIsCurrent(src, dst)) return true;
        diags.error(Span{}, "cannot replace " + dst.string() + " with the current " + what + " " +
                                src.string() + ": " + ec.message() +
                                ". A program using it may still be running.");
        return false;
    }
    return true;
}

bool stageRuntime(const fs::path& runtimeDir, const fs::path& outDir, DiagnosticSink& diags) {
    return stageBeside(runtimeDir / kRuntimeFile, outDir, "the shared runtime", diags);
}

// The C++ runtime the host and the shared runtime were built against, when
// the build put a copy in `redist/` beside the runtime (src/host/CMakeLists.txt
// does on MSVC: msvcp140, msvcp140_2, vcruntime140, vcruntime140_1 — the
// `api-ms-win-crt-*` names are forwarders every supported Windows has). A
// program that carries them runs on a machine without the VC redistributable
// installed. No folder means a build that has nothing to stage — a Linux or
// macOS build, or a host built against the static CRT — and nothing to say.
// A Debug host links the debug CRT, which is not redistributable; the folder
// holds the release one, which such a program does not load.
bool stageRedist(const fs::path& runtimeDir, const fs::path& outDir, DiagnosticSink& diags) {
    std::error_code ec;
    const fs::path redist = runtimeDir / "redist";
    if (!fs::is_directory(redist, ec)) return true;
    for (const auto& entry : fs::directory_iterator(redist, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (!stageBeside(entry.path(), outDir, "the C++ runtime library", diags)) return false;
    }
    return true;
}

}  // namespace

bool linkSharedModule(const brass::object::ObjectFile& obj, const std::string& outputPath,
                      DiagnosticSink& diags, const std::string& entrySymbol) {
    const std::string entry = entrySymbol.empty() ? "bronze_main" : entrySymbol;

    // Every import is a runtime export or a C math function, checked here by
    // the registry rather than left to the loader: an object naming anything
    // else is a bronze whose backend and ABI header disagree, and that is a
    // build error to report at build time.
    std::vector<std::string> runtimeImports;
    std::vector<std::string> mathImports;
    std::string unknown;
    for (const std::string& name : referencedUndefined(obj)) {
        if (runtimeExports().count(name)) runtimeImports.push_back(name);
        else if (mathExports().count(name)) mathImports.push_back(name);
        else unknown += (unknown.empty() ? "" : ", ") + name;
    }
    if (!unknown.empty()) {
        diags.error(Span{}, "the object references " + unknown +
                                ", which the shared bronze runtime does not export (every "
                                "symbol generated code may name is an X(...) line in "
                                "src/abi/bronze_abi.h) and the C math library does not "
                                "provide. The backend and the ABI header this bronze was "
                                "built from disagree.");
        return false;
    }

    const TargetNames names = namesFor(obj.target);
    brass::target::LinkerOptions options;
    options.module_name = fs::path(outputPath).filename().string();
    options.soname = options.module_name;
    options.export_all_functions = false;
    options.explicit_exports = moduleExports(obj, entry);
    if (!runtimeImports.empty()) options.imports.push_back({names.runtimeImport, runtimeImports});
    if (!mathImports.empty()) options.imports.push_back({names.mathImport, mathImports});
    if (names.ownDirRpath) {
        options.rpaths.push_back(names.ownDirRpath);
        // This machine's runtime directory means something only to a module
        // that will run on this machine.
        if (obj.target == brass::Target::host()) {
            if (auto runtimeDir = findSharedRuntimeDir()) {
                options.rpaths.push_back(runtimeDir->string());
            }
        }
    }

    std::error_code ec;
    const fs::path out(outputPath);
    if (out.has_parent_path()) fs::create_directories(out.parent_path(), ec);

    std::string error;
    if (!brass::target::AotLinker::link_to_file(obj, outputPath, options, &error)) {
        diags.error(Span{}, "cannot write the module " + outputPath + ": " +
                                (error.empty() ? std::string("the image writer failed") : error));
        return false;
    }
    return true;
}

bool linkExecutable(const brass::object::ObjectFile& obj, const std::string& outputPath,
                    DiagnosticSink& diags) {
    if (obj.target != brass::Target::host()) {
        diags.error(Span{}, "cannot build a program for another machine: the host binary and the "
                            "shared runtime beside it are this machine's. Build the module alone "
                            "with --emit-shared and pair it with that machine's bronze_host and "
                            "runtime.");
        return false;
    }
    const std::optional<fs::path> runtimeDir = findSharedRuntimeDir();
    if (!runtimeDir) {
        diags.error(Span{}, "cannot build an executable: " + runtimeNotFoundMessage());
        return false;
    }
    const fs::path host = *runtimeDir / kHostFile;
    std::error_code ec;
    if (!fs::exists(host, ec)) {
        diags.error(Span{}, "cannot build an executable: the program host " + host.string() +
                                " is not beside the shared runtime. It is the bronze_host "
                                "target of the tree that built " + kRuntimeFile + ".");
        return false;
    }

    fs::path out = fs::absolute(fs::path(outputPath), ec);
    if (ec) out = fs::path(outputPath);
    const fs::path outDir = out.parent_path().empty() ? fs::current_path() : out.parent_path();
    fs::create_directories(outDir, ec);

    // The module, named so the host's own stem finds it (host_main.cpp).
    fs::path module = out;
    module.replace_extension(namesFor(obj.target).moduleExtension);
    if (!linkSharedModule(obj, module.string(), diags, "bronze_main")) return false;

    // The host, under the program's name.
    if (!copyFileForExec(host, out, ec)) {
        diags.error(Span{}, "cannot write " + out.string() + " (a copy of " + host.string() +
                                "): " + ec.message());
        return false;
    }

    // And the runtime the pair loads, beside them, with the C++ runtime it
    // and the host were built against.
    return stageRuntime(*runtimeDir, outDir, diags) && stageRedist(*runtimeDir, outDir, diags);
}

}  // namespace bronze::cli
