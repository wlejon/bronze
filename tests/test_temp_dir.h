#pragma once

// A scratch directory private to this test process.
//
// Tests that write executables, scripts or module trees must not use fixed
// names directly under the system temp directory: two ctest runs at once (two
// checkouts, or two ctest entries driving the same test binary such as
// oracle-pixi and oracle-pixi-jit) would overwrite and delete each other's
// files. Everything goes under <temp>/bronze-test-<pid>/ instead, which is
// created on first use and removed when the process exits.

#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace bronze_test {

namespace detail {

struct ProcessTempDir {
    std::filesystem::path path;

    ProcessTempDir() {
#ifdef _WIN32
        const long long pid = static_cast<long long>(::_getpid());
#else
        const long long pid = static_cast<long long>(::getpid());
#endif
        path = std::filesystem::temp_directory_path() / ("bronze-test-" + std::to_string(pid));
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        if (ec) {
            throw std::runtime_error("cannot create per-process test temp directory " + path.string() +
                                     ": " + ec.message());
        }
    }

    ~ProcessTempDir() {
        // Best-effort: a child that is still being torn down may hold a file
        // open. A leftover directory is only disk clutter; a later process
        // with the same pid reuses and overwrites it.
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

}  // namespace detail

inline const std::filesystem::path& tempDir() {
    static detail::ProcessTempDir dir;
    return dir.path;
}

}  // namespace bronze_test
