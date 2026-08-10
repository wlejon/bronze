#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "cli/driver.h"

static void writeTestFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

static std::string runAndCaptureOutput(const std::filesystem::path& exePath) {
    std::string result;
#ifdef _WIN32
    FILE* pipe = _popen(exePath.string().c_str(), "r");
#else
    FILE* pipe = popen(exePath.string().c_str(), "r");
#endif
    if (!pipe) return result;
    char buffer[128];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return result;
}

TEST_CASE("CLI driver il command produces canonical IL") {
    std::filesystem::path jsPath = std::filesystem::temp_directory_path() / "test_driver_il.js";
    writeTestFile(jsPath, "function add(a, b) {\n  return a + b;\n}\nadd(10, 20);\n");

    std::string ilOutput;
    int status = bronze::cli::runIl(jsPath.string(), &ilOutput);
    CHECK(status == 0);
    CHECK(ilOutput.find("func add(%0: dynamic, %1: dynamic) -> dynamic") != std::string::npos);
    CHECK(ilOutput.find("func main() -> void") != std::string::npos);

    std::filesystem::remove(jsPath);
}

TEST_CASE("CLI driver build command compiles and links executable") {
    std::filesystem::path jsPath = std::filesystem::temp_directory_path() / "test_driver_build.js";
    std::filesystem::path exePath = std::filesystem::temp_directory_path() / "test_driver_build.exe";

    std::error_code ec;
    if (std::filesystem::exists(exePath, ec)) {
        std::filesystem::remove(exePath, ec);
    }

    writeTestFile(jsPath, "console.log(40 + 2);\n");

    std::string err;
    int status = bronze::cli::runBuild(jsPath.string(), exePath.string(), &err);

#if BRONZE_WITH_LLVM
    REQUIRE(status == 0);
    REQUIRE(std::filesystem::exists(exePath));

    std::string output = runAndCaptureOutput(exePath);
    CHECK(output == "42\n");

    std::filesystem::remove(exePath, ec);
#else
    CHECK(status != 0);
    CHECK(err.find("BRONZE_WITH_LLVM") != std::string::npos);
#endif

    std::filesystem::remove(jsPath, ec);
}
