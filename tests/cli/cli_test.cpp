#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>

#include "../test_temp_dir.h"
#include "cli/driver.h"

static void writeTestFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

// A built program is the executable and the module beside it (cli/link.h);
// both go, so a later build cannot run a stale module under a fresh host.
static void removeProgram(const std::filesystem::path& exe, std::error_code& ec) {
    std::filesystem::remove(exe, ec);
    std::filesystem::path module = exe;
#if defined(_WIN32)
    module.replace_extension(".dll");
#elif defined(__APPLE__)
    module.replace_extension(".dylib");
#else
    module.replace_extension(".so");
#endif
    std::filesystem::remove(module, ec);
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
    std::filesystem::path jsPath = bronze_test::tempDir() / "test_driver_il.js";
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
    std::filesystem::path jsPath = bronze_test::tempDir() / "test_driver_types.js";
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
    std::filesystem::path jsPath = bronze_test::tempDir() / "test_driver_build.js";
    std::filesystem::path exePath = bronze_test::tempDir() / "test_driver_build.exe";

    std::error_code ec;
    removeProgram(exePath, ec);

    writeTestFile(jsPath, "console.log(40 + 2);\n");

    std::string err;
    int status = bronze::cli::runBuild(jsPath.string(), exePath.string(), &err);

    REQUIRE(status == 0);
    REQUIRE(std::filesystem::exists(exePath));

    std::string output = runAndCaptureOutput(exePath);
    CHECK(output == "42\n");

    removeProgram(exePath, ec);

    std::filesystem::remove(jsPath, ec);
}

TEST_CASE("CLI driver concurrent builds do not collide on temp object path") {
    std::filesystem::path dirA = bronze_test::tempDir() / "bronze_test_cli_a";
    std::filesystem::path dirB = bronze_test::tempDir() / "bronze_test_cli_b";
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
    std::filesystem::path jsPath = bronze_test::tempDir() / "test_driver_infer_stats.js";
    std::filesystem::path exePath = bronze_test::tempDir() / "test_driver_infer_stats.exe";
    std::error_code ec;
    removeProgram(exePath, ec);

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

    removeProgram(exePath, ec);
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
    std::filesystem::path jsPath = bronze_test::tempDir() / "test_driver_profile.js";
    std::filesystem::path exePath = bronze_test::tempDir() / "test_driver_profile.exe";
    std::error_code ec;
    removeProgram(exePath, ec);

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

    removeProgram(exePath, ec);
    std::filesystem::remove(jsPath, ec);
}

TEST_CASE("CLI driver accepts --module-root parameter") {
    std::filesystem::path tempDir = bronze_test::tempDir() / "test_cli_modroot";
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

TEST_CASE("CLI driver accepts --import-map parameter in runTypes and runBuild") {
    std::filesystem::path tempDir = bronze_test::tempDir() / "test_cli_importmap";
    std::error_code ec;
    std::filesystem::create_directories(tempDir / "libs" / "addons" / "controls", ec);
    std::filesystem::create_directories(tempDir / "app", ec);

    std::filesystem::path threeFile = tempDir / "libs" / "three.module.js";
    std::filesystem::path controlsFile = tempDir / "libs" / "addons" / "controls" / "OrbitControls.js";
    std::filesystem::path mapFile = tempDir / "importmap.json";
    std::filesystem::path appFile = tempDir / "app" / "main.js";
    std::filesystem::path exePath = tempDir / "app" / "main.exe";

    writeTestFile(threeFile, "export const REVISION = '185';\n");
    writeTestFile(controlsFile, "export function getControlValue() { return 42; }\n");
    writeTestFile(mapFile,
        "{\n"
        "  \"imports\": {\n"
        "    \"three\": \"./libs/three.module.js\",\n"
        "    \"three/addons/\": \"./libs/addons/\"\n"
        "  }\n"
        "}\n");
    writeTestFile(appFile,
        "import { REVISION } from 'three';\n"
        "import { getControlValue } from 'three/addons/controls/OrbitControls.js';\n"
        "console.log(getControlValue());\n");

    std::string typesOutput;
    int status = bronze::cli::runTypes(appFile.string(), &typesOutput, {}, mapFile.string());
    CHECK(status == 0);
    CHECK(typesOutput.find("func mod2.getControlValue()") != std::string::npos);
    CHECK(typesOutput.find("func main()") != std::string::npos);

    std::string err;
    int buildStatus = bronze::cli::runBuild(
        appFile.string(), exePath.string(), &err,
        /*infer=*/true, /*timings=*/false, /*emitObj=*/false,
        /*hostGlobalsPath=*/{}, /*inferStats=*/false, /*statsOut=*/nullptr,
        /*moduleRoots=*/{}, /*entrySymbol=*/{}, /*emitShared=*/false,
        /*retainFnSource=*/true, mapFile.string());

    REQUIRE_MESSAGE(buildStatus == 0, err);
    REQUIRE(std::filesystem::exists(exePath));

    std::string output = runAndCaptureOutput(exePath);
    CHECK(output == "42\n");

    std::string appPathStr = appFile.string();
    std::string mapPathStr = mapFile.string();
    std::string eqFlag = "--import-map=" + mapPathStr;

    // Test runDriver CLI invocations with --import-map
    const char* argv1[] = {"bronze", "types", appPathStr.c_str(), "--import-map", mapPathStr.c_str()};
    CHECK(bronze::cli::runDriver(5, const_cast<char**>(argv1)) == 0);

    const char* argv2[] = {"bronze", "types", appPathStr.c_str(), eqFlag.c_str()};
    CHECK(bronze::cli::runDriver(4, const_cast<char**>(argv2)) == 0);

    // Test error case with missing import map file
    const char* argvBad[] = {"bronze", "types", appPathStr.c_str(), "--import-map", "nonexistent.json"};
    CHECK(bronze::cli::runDriver(5, const_cast<char**>(argvBad)) != 0);

    std::filesystem::remove_all(tempDir, ec);
}

// Error.stack positions come from the module's line index, which outlives the
// source texts `--no-fn-source` drops: the two builds of one program print the
// same frames, at the lines and columns the program was written on, rather
// than the no-source build degrading every frame to 1:1.
// A built program has no compiler beside it, so CreateDynamicFunction — the
// call form of %Function%, %GeneratorFunction%, %AsyncFunction% and
// %AsyncGeneratorFunction%, and `eval` — is a TypeError naming the kind. The
// oracle suite cannot pin this: node compiles the source, and so does
// `bronze run`, whose evaluator answers the runtime's dynamic-code hooks.
TEST_CASE("A built program refuses dynamic code compilation with a TypeError") {
    std::filesystem::path jsPath = bronze_test::tempDir() / "test_driver_dynamic_fn.js";
    std::filesystem::path exePath = bronze_test::tempDir() / "test_driver_dynamic_fn.exe";
    std::error_code ec;
    removeProgram(exePath, ec);

    writeTestFile(jsPath,
        "function* gen() {}\n"
        "async function af() {}\n"
        "async function* agen() {}\n"
        "const kinds = [Function, Object.getPrototypeOf(gen).constructor,\n"
        "               Object.getPrototypeOf(af).constructor,\n"
        "               Object.getPrototypeOf(agen).constructor];\n"
        "for (const K of kinds) {\n"
        "  try { K('return 1'); console.log('compiled'); }\n"
        "  catch (e) { console.log(e.name + ': ' + e.message.split(':')[0]); }\n"
        "}\n"
        "try { eval('1'); console.log('compiled'); }\n"
        "catch (e) { console.log(e.name + ': ' + e.message.split(':')[0]); }\n"
    );

    std::string err;
    int status = bronze::cli::runBuild(jsPath.string(), exePath.string(), &err);
    REQUIRE_MESSAGE(status == 0, err);

    CHECK(runAndCaptureOutput(exePath) ==
          "TypeError: Function\n"
          "TypeError: GeneratorFunction\n"
          "TypeError: AsyncFunction\n"
          "TypeError: AsyncGeneratorFunction\n"
          "TypeError: eval\n");

    removeProgram(exePath, ec);
    std::filesystem::remove(jsPath, ec);
}

TEST_CASE("CLI driver --no-fn-source keeps the Error.stack line table") {
    std::filesystem::path jsPath = bronze_test::tempDir() / "test_driver_no_fn_source.js";
    std::filesystem::path exeWith = bronze_test::tempDir() / "test_driver_fn_source.exe";
    std::filesystem::path exeWithout = bronze_test::tempDir() / "test_driver_no_fn_source.exe";
    std::error_code ec;
    removeProgram(exeWith, ec);
    removeProgram(exeWithout, ec);

    writeTestFile(jsPath,
        "function inner() {\n"
        "  return new Error('where');\n"
        "}\n"
        "function outer() {\n"
        "  return inner();\n"
        "}\n"
        "const e = outer();\n"
        "console.log(e.stack.split('\\n').slice(1, 3).map(l => l.replace(/\\(.*[\\\\/]/, '(')).join('|'));\n"
    );

    std::string err;
    int status = bronze::cli::runBuild(jsPath.string(), exeWith.string(), &err);
    REQUIRE_MESSAGE(status == 0, err);
    status = bronze::cli::runBuild(
        jsPath.string(), exeWithout.string(), &err,
        /*infer=*/true, /*timings=*/false, /*emitObj=*/false,
        /*hostGlobalsPath=*/{}, /*inferStats=*/false, /*statsOut=*/nullptr,
        /*moduleRoots=*/{}, /*entrySymbol=*/{}, /*emitShared=*/false,
        /*retainFnSource=*/false);
    REQUIRE_MESSAGE(status == 0, err);

    const std::string withSource = runAndCaptureOutput(exeWith);
    const std::string withoutSource = runAndCaptureOutput(exeWithout);
    CHECK(withSource ==
          "    at inner (test_driver_no_fn_source.js:2:10)|"
          "    at outer (test_driver_no_fn_source.js:5:10)\n");
    CHECK(withoutSource == withSource);

    removeProgram(exeWith, ec);
    removeProgram(exeWithout, ec);
    std::filesystem::remove(jsPath, ec);
}

// `--target` writes another machine's module from this one — brass's image
// writers need no cross toolchain — and refuses a program, which needs that
// machine's host binary. The x64 macOS module is the one the codegen had to
// become position independent for: dyld will not slide a pointer in __TEXT.
TEST_CASE("CLI driver --target writes a module for another machine and refuses a program") {
    std::filesystem::path jsPath = bronze_test::tempDir() / "test_driver_target.js";
    std::filesystem::path dylibPath = bronze_test::tempDir() / "test_driver_target.dylib";
    std::filesystem::path soPath = bronze_test::tempDir() / "test_driver_target.so";
    std::filesystem::path exePath = bronze_test::tempDir() / "test_driver_target.exe";
    std::error_code ec;
    std::filesystem::remove(dylibPath, ec);
    std::filesystem::remove(soPath, ec);
    removeProgram(exePath, ec);

    writeTestFile(jsPath,
        "class P { constructor(x) { this.x = x; } get d() { return this.x * 2; } }\n"
        "const xs = [1, 2, 3].map(x => new P(x).d);\n"
        "console.log(xs.join(','), Math.sqrt(xs[2]));\n");

    auto readMagic = [](const std::filesystem::path& p) -> uint32_t {
        std::ifstream in(p, std::ios::binary);
        unsigned char b[4] = {0, 0, 0, 0};
        in.read(reinterpret_cast<char*>(b), 4);
        return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
    };

    std::string err;
    int status = bronze::cli::runBuild(
        jsPath.string(), dylibPath.string(), &err,
        /*infer=*/true, /*timings=*/false, /*emitObj=*/false,
        /*hostGlobalsPath=*/{}, /*inferStats=*/false, /*statsOut=*/nullptr,
        /*moduleRoots=*/{}, /*entrySymbol=*/{}, /*emitShared=*/true,
        /*retainFnSource=*/true, /*importMapPath=*/{}, /*assumeNoBigInt=*/false,
        /*pinsPath=*/{}, /*censusOutPath=*/{}, /*pinsAllowObserved=*/false,
        /*nativeManifestPath=*/{}, /*nativeLibPath=*/{}, /*entryResolvesAs=*/{},
        /*targetName=*/"x64-macos");
    REQUIRE_MESSAGE(status == 0, err);
    REQUIRE(std::filesystem::exists(dylibPath));
    CHECK(readMagic(dylibPath) == 0xFEEDFACFu);   // MH_MAGIC_64

    status = bronze::cli::runBuild(
        jsPath.string(), soPath.string(), &err,
        /*infer=*/true, /*timings=*/false, /*emitObj=*/false,
        /*hostGlobalsPath=*/{}, /*inferStats=*/false, /*statsOut=*/nullptr,
        /*moduleRoots=*/{}, /*entrySymbol=*/{}, /*emitShared=*/true,
        /*retainFnSource=*/true, /*importMapPath=*/{}, /*assumeNoBigInt=*/false,
        /*pinsPath=*/{}, /*censusOutPath=*/{}, /*pinsAllowObserved=*/false,
        /*nativeManifestPath=*/{}, /*nativeLibPath=*/{}, /*entryResolvesAs=*/{},
        /*targetName=*/"x64-linux");
    REQUIRE_MESSAGE(status == 0, err);
    CHECK(readMagic(soPath) == 0x464C457Fu);      // \x7fELF

#if defined(__APPLE__)
    const char* foreignTarget = "x64-linux";
#else
    const char* foreignTarget = "x64-macos";
#endif
    // A program for another machine, and an unknown machine: refused by name.
    err.clear();
    status = bronze::cli::runBuild(
        jsPath.string(), exePath.string(), &err,
        /*infer=*/true, /*timings=*/false, /*emitObj=*/false,
        /*hostGlobalsPath=*/{}, /*inferStats=*/false, /*statsOut=*/nullptr,
        /*moduleRoots=*/{}, /*entrySymbol=*/{}, /*emitShared=*/false,
        /*retainFnSource=*/true, /*importMapPath=*/{}, /*assumeNoBigInt=*/false,
        /*pinsPath=*/{}, /*censusOutPath=*/{}, /*pinsAllowObserved=*/false,
        /*nativeManifestPath=*/{}, /*nativeLibPath=*/{}, /*entryResolvesAs=*/{},
        /*targetName=*/foreignTarget);
    CHECK(status != 0);
    CHECK(err.find("--emit-shared") != std::string::npos);
    CHECK(!std::filesystem::exists(exePath));

    std::string jsPathStr = jsPath.string();
    std::string outStr = soPath.string();
    const char* argvBad[] = {"bronze", "build", jsPathStr.c_str(), "--emit-shared",
                             "--target", "riscv-plan9", "-o", outStr.c_str()};
    CHECK(bronze::cli::runDriver(8, const_cast<char**>(argvBad)) != 0);

    std::filesystem::remove(dylibPath, ec);
    std::filesystem::remove(soPath, ec);
    std::filesystem::remove(jsPath, ec);
}
