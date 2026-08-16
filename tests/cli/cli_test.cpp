#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>

#include "cli/driver.h"

static void writeTestFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

[[maybe_unused]] static std::string runAndCaptureOutput(const std::filesystem::path& exePath) {
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

    // The CLI is the composition root: it runs inference and hands the side
    // table to lowering. `add` is direct-callable and its only call site passes
    // numbers, so signature specialization gives it an unboxed f64 signature
    // and the call site a direct typed call.
    std::string ilOutput;
    int status = bronze::cli::runIl(jsPath.string(), &ilOutput);
    CHECK(status == 0);
    CHECK(ilOutput.find("func add(%0: f64, %1: f64) -> f64") != std::string::npos);
    CHECK(ilOutput.find("func main() -> void") != std::string::npos);
    CHECK(ilOutput.find("box.f64") == std::string::npos);

    // The same source with inference switched off must reproduce the
    // pre-inference lowering exactly: everything dynamic, every argument boxed.
    // Pinning both is what makes the switch a ratchet rather than a comfort
    // blanket.
    std::string noInferOutput;
    status = bronze::cli::runIl(jsPath.string(), &noInferOutput, /*infer=*/false);
    CHECK(status == 0);
    CHECK(noInferOutput.find("func add(%0: dynamic, %1: dynamic) -> dynamic") !=
          std::string::npos);
    CHECK(noInferOutput.find("box.f64") != std::string::npos);

    std::filesystem::remove(jsPath);
}

TEST_CASE("CLI driver types command produces the canonical type dump") {
    std::filesystem::path jsPath = std::filesystem::temp_directory_path() / "test_driver_types.js";
    writeTestFile(jsPath, "function add(a, b) {\n  return a + b;\n}\nadd(10, 20);\n");

    std::string typesOutput;
    int status = bronze::cli::runTypes(jsPath.string(), &typesOutput);
    CHECK(status == 0);
    CHECK(typesOutput.find("func add(a: number, b: number) -> number direct-callable") !=
          std::string::npos);
    CHECK(typesOutput.find("func main() -> undefined") != std::string::npos);

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

#if BRONZE_WITH_LLVM
TEST_CASE("CLI driver concurrent builds do not collide on temp object path") {
    std::filesystem::path dirA = std::filesystem::temp_directory_path() / "bronze_test_cli_a";
    std::filesystem::path dirB = std::filesystem::temp_directory_path() / "bronze_test_cli_b";
    std::error_code ec;
    std::filesystem::create_directories(dirA, ec);
    std::filesystem::create_directories(dirB, ec);

    std::filesystem::path jsPathA = dirA / "main.js";
    std::filesystem::path jsPathB = dirB / "main.js";
    std::filesystem::path exePathA = dirA / "main.exe";
    std::filesystem::path exePathB = dirB / "main.exe";

    writeTestFile(jsPathA, "console.log(101);\n");
    writeTestFile(jsPathB, "console.log(202);\n");

    std::string errA, errB;
    auto futA = std::async(std::launch::async, [&] {
        return bronze::cli::runBuild(jsPathA.string(), exePathA.string(), &errA);
    });
    auto futB = std::async(std::launch::async, [&] {
        return bronze::cli::runBuild(jsPathB.string(), exePathB.string(), &errB);
    });

    int statusA = futA.get();
    int statusB = futB.get();

    REQUIRE_MESSAGE(statusA == 0, errA);
    REQUIRE_MESSAGE(statusB == 0, errB);
    CHECK(runAndCaptureOutput(exePathA) == "101\n");
    CHECK(runAndCaptureOutput(exePathB) == "202\n");

    std::filesystem::remove_all(dirA, ec);
    std::filesystem::remove_all(dirB, ec);
}

TEST_CASE("CLI driver --infer-stats produces deterministic stats output") {
    std::filesystem::path jsPath = std::filesystem::temp_directory_path() / "test_driver_infer_stats.js";
    std::filesystem::path exePath = std::filesystem::temp_directory_path() / "test_driver_infer_stats.exe";
    std::error_code ec;
    if (std::filesystem::exists(exePath, ec)) std::filesystem::remove(exePath, ec);

    writeTestFile(jsPath,
        "function add(a, b) {\n"
        "  return a + b;\n"
        "}\n"
        "const obj = { x: 1, y: 2 };\n"
        "const k = 'x';\n"
        "const r = obj.x + obj[k];\n"
        "add(r, 10);\n"
    );

    std::string err;
    std::string statsOut;
    int status = bronze::cli::runBuild(jsPath.string(), exePath.string(), &err,
                                       /*infer=*/true, /*timings=*/false, /*emitObj=*/false,
                                       /*hostGlobalsPath=*/{}, /*inferStats=*/true,
                                       &statsOut);

    REQUIRE_MESSAGE(status == 0, err);
    CHECK(statsOut.find("=== Inference Statistics ===") != std::string::npos);
    CHECK(statsOut.find("Property Accesses:") != std::string::npos);
    CHECK(statsOut.find("Calls:") != std::string::npos);
    CHECK(statsOut.find("Element Operations:") != std::string::npos);
    CHECK(statsOut.find("Total:") != std::string::npos);

    if (std::filesystem::exists(exePath, ec)) std::filesystem::remove(exePath, ec);
    std::filesystem::remove(jsPath, ec);
}

static std::string runAndCaptureStderr(const std::filesystem::path& exePath, const char* envVar = nullptr) {
#ifdef _WIN32
    if (envVar) {
        _putenv(envVar);
    } else {
        _putenv("BRONZE_PROFILE=");
    }
    std::string cmd = "\"" + exePath.string() + "\" 2>&1";
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    if (envVar) {
        putenv(const_cast<char*>(envVar));
    } else {
        unsetenv("BRONZE_PROFILE");
    }
    std::string cmd = exePath.string() + " 2>&1";
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    std::string result;
    if (!pipe) return result;
    char buffer[128];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
#ifdef _WIN32
    _pclose(pipe);
    _putenv("BRONZE_PROFILE=");
#else
    pclose(pipe);
    unsetenv("BRONZE_PROFILE");
#endif
    return result;
}

TEST_CASE("BRONZE_PROFILE=1 runtime profile outputs helper table on stderr") {
    std::filesystem::path jsPath = std::filesystem::temp_directory_path() / "test_driver_profile.js";
    std::filesystem::path exePath = std::filesystem::temp_directory_path() / "test_driver_profile.exe";
    std::error_code ec;
    if (std::filesystem::exists(exePath, ec)) std::filesystem::remove(exePath, ec);

    writeTestFile(jsPath,
        "const o = { a: 1 };\n"
        "console.log(o.a);\n"
    );

    std::string err;
    int status = bronze::cli::runBuild(jsPath.string(), exePath.string(), &err);
    REQUIRE_MESSAGE(status == 0, err);

    std::string stderrOutput = runAndCaptureStderr(exePath, "BRONZE_PROFILE=1");
    CHECK(stderrOutput.find("=== Bronze Runtime Profile (BRONZE_PROFILE=1) ===") != std::string::npos);
    CHECK(stderrOutput.find("Total Dynamic ABI Helper Invocations:") != std::string::npos);

    std::string normalStderr = runAndCaptureStderr(exePath, nullptr);
    CHECK(normalStderr.find("Bronze Runtime Profile") == std::string::npos);

    if (std::filesystem::exists(exePath, ec)) std::filesystem::remove(exePath, ec);
    std::filesystem::remove(jsPath, ec);
}

TEST_CASE("CLI driver accepts --module-root parameter") {
    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "test_cli_modroot";
    std::error_code ec;
    std::filesystem::create_directories(tempDir / "lib", ec);
    std::filesystem::create_directories(tempDir / "app", ec);

    std::filesystem::path libFile = tempDir / "lib" / "helper.js";
    std::filesystem::path appFile = tempDir / "app" / "main.js";
    writeTestFile(libFile, "export function helper() { return 99; }\n");
    writeTestFile(appFile, "import { helper } from '/lib/helper.js';\nconsole.log(helper());\n");

    std::string typesOutput;
    int status = bronze::cli::runTypes(appFile.string(), &typesOutput, {{"/lib", tempDir / "lib"}});
    CHECK(status == 0);
    CHECK(typesOutput.find("func mod1.helper()") != std::string::npos);
    CHECK(typesOutput.find("func main()") != std::string::npos);

    std::filesystem::remove_all(tempDir, ec);
}
#endif



