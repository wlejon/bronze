#include "cli/driver_helpers.h"
#include "cli/driver.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string_view>

namespace bronze::cli {

int fail(const std::string& message) {
    std::fputs(message.c_str(), stderr);
    return 1;
}

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

bool loadPins(const std::string& path, types::PinManifest& out, std::string& err,
              bool allowObserved) {
    std::string text;
    if (!readFile(path, text)) {
        err = "error: cannot read pin manifest " + path + "\n";
        return false;
    }
    return out.parse(text, path, err, allowObserved);
}

bool hasHostBoundary(const std::string& hostGlobalsPath, bool emitObj, bool emitShared) {
    return !hostGlobalsPath.empty() || emitObj || emitShared;
}

bool parseTargetName(const std::string& name, brass::Target& out, std::string& err,
                     brass::object::MachOBuildVersion* machoVersion) {
    namespace macho = brass::object::macho;
    // A Darwin target's version and simulator suffixes.
    const std::string_view full(name);
    for (const char* arch : {"x64", "aarch64"}) {
        const std::string prefix = std::string(arch) + "-";
        if (full.substr(0, prefix.size()) != prefix) continue;
        std::string_view os = full.substr(prefix.size());
        brass::object::MachOBuildVersion bv;
        if (os.substr(0, 5) == "macos") {
            os.remove_prefix(5);
        } else if (os.substr(0, 3) == "ios") {
            os.remove_prefix(3);
            bv.platform = macho::PLATFORM_IOS;
            constexpr std::string_view kSim = "-simulator";
            if (os.size() >= kSim.size() && os.substr(os.size() - kSim.size()) == kSim) {
                os.remove_suffix(kSim.size());
                bv.platform = macho::PLATFORM_IOSSIMULATOR;
            }
        } else {
            break;
        }
        if (!os.empty()) {
            const auto v = brass::object::MachOBuildVersion::parse_version(os);
            if (!v) {
                err = "error: --target " + name + ": '" + std::string(os) +
                      "' is not a version (X, X.Y or X.Y.Z)\n";
                return false;
            }
            bv.minos = *v;
        } else if (bv.platform == macho::PLATFORM_MACOS) {
            break;  // plain x64-macos / aarch64-macos: the table below
        }
        if (bv.platform == macho::PLATFORM_IOS && std::string_view(arch) == "x64") {
            err = "error: --target " + name + ": x64 iOS is the simulator only (x64-ios-simulator)\n";
            return false;
        }
        out = std::string_view(arch) == "x64" ? brass::Target::x64_macos() : brass::Target::aarch64_macos();
        if (machoVersion) *machoVersion = bv;
        return true;
    }
    static const struct { const char* name; brass::Target target; } kTargets[] = {
        {"x64-windows", brass::Target::x64_windows()},
        {"x64-linux", brass::Target::x64_linux()},
        {"x64-macos", brass::Target::x64_macos()},
        {"aarch64-linux", brass::Target::aarch64_linux()},
        {"aarch64-macos", brass::Target::aarch64_macos()},
        {"aarch64-windows", brass::Target::aarch64_windows()},
    };
    for (const auto& t : kTargets) {
        if (name == t.name) {
            out = t.target;
            return true;
        }
    }
    err = "error: unknown --target " + name + "; one of";
    for (const auto& t : kTargets) err += std::string(" ") + t.name;
    err += " (a Darwin one may add a minimum version, aarch64-macos13.0; iOS is aarch64-ios[<version>]"
           " or <arch>-ios[<version>]-simulator)\n";
    return false;
}

int dispatchProveEquivArgs(int argc, char** argv) {
    std::string oldPath;
    std::string newPath;
    std::string fnFilter;
    std::string importMapPath;
    std::vector<modules::ModuleRoot> moduleRoots;
    bool jsonOutput = false;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--json") {
            jsonOutput = true;
        } else if (arg == "--fn") {
            if (i + 1 < argc) {
                fnFilter = argv[++i];
            } else {
                return fail("error: missing argument for --fn\n");
            }
        } else if (arg.rfind("--fn=", 0) == 0) {
            fnFilter = arg.substr(5);
            if (fnFilter.empty()) return fail("error: missing argument for --fn\n");
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
        } else if (oldPath.empty()) {
            oldPath = arg;
        } else if (newPath.empty()) {
            newPath = arg;
        } else {
            return fail("error: unexpected argument " + arg + "\n");
        }
    }
    if (oldPath.empty() || newPath.empty()) {
        return fail("error: usage: bronze prove-equiv <old.js> <new.js> [--fn <name>] [--json]\n");
    }
    return runProveEquivCmd(oldPath, newPath, fnFilter, jsonOutput, nullptr, moduleRoots, importMapPath);
}

int dispatchSynthTestsArgs(int argc, char** argv) {
    std::string inputPath;
    std::string outputPath;
    std::string importMapPath;
    std::vector<modules::ModuleRoot> moduleRoots;
    bool jsonOutput = false;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--json") {
            jsonOutput = true;
        } else if (arg == "-o") {
            if (i + 1 < argc) {
                outputPath = argv[++i];
            } else {
                return fail("error: missing argument for -o\n");
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
        } else if (arg == "--import-map") {
            if (i + 1 < argc) {
                importMapPath = argv[++i];
            } else {
                return fail("error: missing argument for --import-map\n");
            }
        } else if (arg.rfind("--import-map=", 0) == 0) {
            importMapPath = arg.substr(13);
            if (importMapPath.empty()) return fail("error: missing argument for --import-map\n");
        } else if (inputPath.empty()) {
            inputPath = arg;
        } else {
            return fail("error: unexpected argument " + arg + "\n");
        }
    }
    if (inputPath.empty()) {
        return fail("error: usage: bronze synth-tests <file.js> [-o <out.js>] [--json]\n");
    }
    return runSynthTestsCmd(inputPath, outputPath, jsonOutput, nullptr, moduleRoots, importMapPath);
}

}  // namespace bronze::cli
