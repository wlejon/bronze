#include "cli/native_manifest_resolve.h"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace bronze::cli {
namespace {

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

}  // namespace

std::optional<lower::NativeManifest> resolveNativeManifest(
    const std::string& manifestPath,
    const std::string& libPath,
    std::string& err) {

    std::filesystem::path mPath = manifestPath;
    if (mPath.empty()) {
        if (auto env = envPath("BRONZE_NATIVE_MANIFEST")) {
            mPath = *env;
        }
    }
    if (mPath.empty()) {
        const std::vector<std::filesystem::path> manifestCandidates = {
            "D:/projects/brosurface/out/c_abi/manifest",
            "../brosurface/out/c_abi/manifest",
            "../../brosurface/out/c_abi/manifest",
        };
        for (const auto& cand : manifestCandidates) {
            std::error_code ec;
            if (std::filesystem::is_directory(cand, ec)) {
                mPath = cand;
                break;
            }
        }
    }

    std::optional<lower::NativeManifest> manifest;
    if (!mPath.empty()) {
        std::error_code ec;
        if (std::filesystem::is_directory(mPath, ec)) {
            manifest = lower::NativeManifest::loadFromDirectory(mPath.string(), err);
        } else if (std::filesystem::exists(mPath, ec)) {
            manifest = lower::NativeManifest::loadFromFile(mPath.string(), err);
        } else if (!manifestPath.empty()) {
            err = "error: native manifest path does not exist: " + manifestPath + "\n";
            return std::nullopt;
        }
        if (!manifest) {
            return std::nullopt;
        }
    }

    if (manifest) {
        std::filesystem::path lPath = libPath;
        if (lPath.empty()) {
            if (auto env = envPath("BRONZE_NATIVE_LIB")) {
                lPath = *env;
            }
        }
        if (lPath.empty()) {
            const std::vector<std::filesystem::path> libCandidates = {
                "D:/projects/bro/build/src/c_abi/Release/bro_c_abi.lib",
                "D:/projects/bro/build/src/c_abi/Debug/bro_c_abi.lib",
                "../bro/build/src/c_abi/Release/bro_c_abi.lib",
                "../bro/build/src/c_abi/Debug/bro_c_abi.lib",
                "../../bro/build/src/c_abi/Release/bro_c_abi.lib",
                "../../bro/build/src/c_abi/Debug/bro_c_abi.lib",
            };
            for (const auto& cand : libCandidates) {
                std::error_code ec;
                if (std::filesystem::exists(cand, ec)) {
                    lPath = cand;
                    break;
                }
            }
        }
        if (!lPath.empty()) {
            std::error_code ec;
            if (std::filesystem::exists(lPath, ec)) {
                manifest->addExtraLibPath(std::filesystem::canonical(lPath, ec).string());

                // Locate the build directory containing CMakeCache.txt to find companion libs
                std::filesystem::path buildDir = lPath.parent_path();
                while (!buildDir.empty() && buildDir.has_relative_path()) {
                    if (std::filesystem::exists(buildDir / "CMakeCache.txt", ec)) {
                        break;
                    }
                    auto parent = buildDir.parent_path();
                    if (parent == buildDir) break;
                    buildDir = parent;
                }

                if (std::filesystem::exists(buildDir / "CMakeCache.txt", ec)) {
                    std::string config = lPath.parent_path().filename().string();
                    std::vector<std::filesystem::path> companions = {
                        buildDir / "FastNoise2" / "src" / config / (config == "Debug" ? "FastNoiseD.lib" : "FastNoise.lib"),
                        buildDir / "FastNoise2" / "src" / "FastNoise.lib",
                        buildDir / "FastNoise2" / "src" / "FastSIMD_FastNoise.dir" / config / "FastSIMD_FastNoise.lib",
                        buildDir / "FastNoise2" / "src" / "FastSIMD_FastNoise.dir" / "FastSIMD_FastNoise.lib",
                        buildDir / "_deps" / "fastsimd-build" / "FastSIMD.dir" / config / "FastSIMD.lib",
                        buildDir / "_deps" / "fastsimd-build" / "FastSIMD.dir" / "FastSIMD.lib",
                    };
                    for (const auto& comp : companions) {
                        if (std::filesystem::exists(comp, ec)) {
                            manifest->addExtraLibPath(std::filesystem::canonical(comp, ec).string());
                        }
                    }
                }
            } else if (!libPath.empty()) {
                err = "error: native lib path does not exist: " + libPath + "\n";
                return std::nullopt;
            }
        }
    }

    return manifest;
}

}  // namespace bronze::cli
