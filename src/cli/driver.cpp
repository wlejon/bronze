#include "cli/driver.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "ast/dump.h"
#include "codegen/backend.h"
#if BRONZE_WITH_LLVM
#include "codegen-llvm/llvm_backend.h"
#endif
#include "il/il.h"
#include "il/print.h"
#include "lex/lexer.h"
#include "lower/lower.h"
#include "parse/parser.h"
#include "support/diagnostics.h"
#include "types/dump.h"
#include "types/infer.h"

namespace bronze::cli {
namespace {

constexpr const char* kUsage =
    "bronze — AOT compiler for JavaScript (native-first, LLVM backend)\n"
    "\n"
    "Usage:\n"
    "  bronze lex <file>                   Tokenize and print one token per line\n"
    "  bronze parse <file>                 Parse and print the canonical AST dump\n"
    "  bronze types <file>                 Infer types and print the canonical type dump\n"
    "  bronze il <file>                    Lower to IL and print canonical IL dump\n"
    "  bronze build <file> -o <output>     Compile JS source to native executable\n"
    "  bronze version                      Print version\n"
    "\n"
    "Options (il, build):\n"
    "  --no-infer                          Skip inference; lower everything on the\n"
    "                                      uniform dynamic convention (docs/0010\n"
    "                                      decision 8 — the bisection seam)\n";

int fail(const std::string& message) {
    std::fputs(message.c_str(), stderr);
    return 1;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

std::filesystem::path getExecutableDir() {
#ifdef _WIN32
    char buffer[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, buffer, MAX_PATH);
    if (len > 0) {
        return std::filesystem::path(buffer).parent_path();
    }
#endif
    return std::filesystem::current_path();
}

std::optional<std::filesystem::path> findRuntimeLib() {
    if (const char* envPath = std::getenv("BRONZE_RT_LIB")) {
        std::filesystem::path p(envPath);
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return p;
    }

    std::vector<std::filesystem::path> candidates;
    std::filesystem::path exeDir = getExecutableDir();

    candidates.push_back(exeDir / "bronze_rt.lib");
    candidates.push_back(exeDir / "../rt/bronze_rt.lib");
    candidates.push_back(exeDir / "src/rt/bronze_rt.lib");
    candidates.push_back(exeDir / "../../src/rt/bronze_rt.lib");

    std::filesystem::path cwd = std::filesystem::current_path();
    candidates.push_back(cwd / "bronze_rt.lib");
    candidates.push_back(cwd / "src/rt/bronze_rt.lib");
    candidates.push_back(cwd / "build/dev/src/rt/bronze_rt.lib");

    for (const auto& cand : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(cand, ec)) {
            return std::filesystem::canonical(cand, ec);
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> findRuntimeCpp() {
    std::vector<std::filesystem::path> candidates;
    std::filesystem::path exeDir = getExecutableDir();

    candidates.push_back(exeDir / "../../src/rt/rt.cpp");
    candidates.push_back(exeDir / "../src/rt/rt.cpp");
    candidates.push_back(exeDir / "src/rt/rt.cpp");

    std::filesystem::path cwd = std::filesystem::current_path();
    candidates.push_back(cwd / "src/rt/rt.cpp");

    for (const auto& cand : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(cand, ec)) {
            return std::filesystem::canonical(cand, ec);
        }
    }
    return std::nullopt;
}

bool linkExecutable(const std::string& objPath, const std::string& outputPath, DiagnosticSink& diags) {
    auto rtLib = findRuntimeLib();
    auto rtCpp = findRuntimeCpp();

    if (!rtLib && !rtCpp) {
        diags.error(Span{}, "Runtime library (bronze_rt.lib) or runtime source (rt.cpp) not found");
        return false;
    }

    std::vector<std::string> commands;

    if (rtLib) {
        std::string libStr = rtLib->string();
        std::filesystem::path runtimeLibPath = rtLib->parent_path() / "bronze_runtime.lib";
        if (!std::filesystem::exists(runtimeLibPath)) {
            runtimeLibPath = rtLib->parent_path() / "../runtime/bronze_runtime.lib";
        }
        std::string runtimeLibStr = std::filesystem::exists(runtimeLibPath) ? ("\"" + runtimeLibPath.string() + "\"") : "";

        commands.push_back("lld-link /nologo /subsystem:console /include:main /out:\"" + outputPath + "\" \"" + objPath + "\" \"" + libStr + "\" " + runtimeLibStr);
        commands.push_back("lld-link /nologo /subsystem:console /wholearchive:\"" + libStr + "\" " + (runtimeLibStr.empty() ? "" : "/wholearchive:" + runtimeLibStr) + " /out:\"" + outputPath + "\" \"" + objPath + "\"");
        commands.push_back("link.exe /nologo /subsystem:console /include:main /out:\"" + outputPath + "\" \"" + objPath + "\" \"" + libStr + "\" " + runtimeLibStr);
        commands.push_back("link.exe /nologo /subsystem:console /wholearchive:\"" + libStr + "\" " + (runtimeLibStr.empty() ? "" : "/wholearchive:" + runtimeLibStr) + " /out:\"" + outputPath + "\" \"" + objPath + "\"");
        commands.push_back("clang-cl /nologo \"" + objPath + "\" \"" + libStr + "\" " + runtimeLibStr + " /link /include:main /Fe:\"" + outputPath + "\"");
        commands.push_back("cl.exe /nologo \"" + objPath + "\" \"" + libStr + "\" " + runtimeLibStr + " /link /include:main /Fe:\"" + outputPath + "\"");
    }

    if (rtCpp) {
        std::string cppStr = rtCpp->string();
        commands.push_back("clang-cl /nologo /std:c++20 \"" + objPath + "\" \"" + cppStr + "\" /Fe:\"" + outputPath + "\"");
        commands.push_back("cl.exe /nologo /std:c++20 \"" + objPath + "\" \"" + cppStr + "\" /Fe:\"" + outputPath + "\"");
    }

    for (const auto& cmd : commands) {
        int res = std::system(cmd.c_str());
        std::error_code ec;
        if (res == 0 && std::filesystem::exists(outputPath, ec)) {
            return true;
        }
    }

    diags.error(Span{}, "Failed to link executable with available toolchain (lld-link, link, clang-cl, cl)");
    return false;
}

}  // namespace

int runTypes(const std::string& sourcePath, std::string* outString) {
    std::string text;
    if (!readFile(sourcePath, text)) {
        std::string msg = "error: cannot read " + sourcePath + "\n";
        if (outString) *outString = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    SourceBuffer buffer(sourcePath, std::move(text));
    DiagnosticSink diags;

    auto tokens = Lexer(buffer, diags).lex();
    if (diags.hasErrors()) {
        std::string msg = diags.render(buffer);
        if (outString) *outString = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    auto astModule = Parser(std::move(tokens), diags).parseModule(sourcePath);
    if (diags.hasErrors() || !astModule) {
        std::string msg = diags.render(buffer);
        if (outString) *outString = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    auto inferred = types::inferModule(*astModule, diags);
    if (diags.hasErrors() || !inferred) {
        std::string msg = diags.render(buffer);
        if (outString) *outString = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    std::string printed = types::dump(*inferred);
    if (outString) {
        *outString = printed;
    } else {
        std::fputs(printed.c_str(), stdout);
    }
    return 0;
}

int runIl(const std::string& sourcePath, std::string* outString, bool infer) {
    std::string text;
    if (!readFile(sourcePath, text)) {
        std::string msg = "error: cannot read " + sourcePath + "\n";
        if (outString) *outString = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    SourceBuffer buffer(sourcePath, std::move(text));
    DiagnosticSink diags;

    auto tokens = Lexer(buffer, diags).lex();
    if (diags.hasErrors()) {
        std::string msg = diags.render(buffer);
        if (outString) *outString = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    auto astModule = Parser(std::move(tokens), diags).parseModule(sourcePath);
    if (diags.hasErrors() || !astModule) {
        std::string msg = diags.render(buffer);
        if (outString) *outString = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    // The CLI is the composition root: it runs inference and hands the side
    // table to lowering (docs/0010 decision 1). A null side table is the
    // no-inference mode, not a failure mode — inference itself only ever
    // fails on an internal impossibility, which is diagnosed and fatal.
    std::optional<types::InferenceResult> inferred;
    if (infer) {
        inferred = types::inferModule(*astModule, diags);
        if (diags.hasErrors() || !inferred) {
            std::string msg = diags.render(buffer);
            if (outString) *outString = msg;
            else std::fputs(msg.c_str(), stderr);
            return 1;
        }
    }

    auto ilModule = lower::lowerModule(*astModule, diags,
                                       inferred ? &*inferred : nullptr);
    if (diags.hasErrors() || !ilModule) {
        std::string msg = diags.render(buffer);
        if (outString) *outString = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    std::string printed = il::print(*ilModule);
    if (outString) {
        *outString = printed;
    } else {
        std::fputs(printed.c_str(), stdout);
    }
    return 0;
}

int runBuild(const std::string& sourcePath, const std::string& outputPath, std::string* errOut,
             bool infer) {
#if !BRONZE_WITH_LLVM
    (void)infer;
    std::string msg = "error: bronze build requires LLVM backend (BRONZE_WITH_LLVM=ON)\n";
    if (errOut) *errOut = msg;
    else std::fputs(msg.c_str(), stderr);
    return 1;
#else
    std::string text;
    if (!readFile(sourcePath, text)) {
        std::string msg = "error: cannot read " + sourcePath + "\n";
        if (errOut) *errOut = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    SourceBuffer buffer(sourcePath, std::move(text));
    DiagnosticSink diags;

    auto tokens = Lexer(buffer, diags).lex();
    if (diags.hasErrors()) {
        std::string msg = diags.render(buffer);
        if (errOut) *errOut = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    auto astModule = Parser(std::move(tokens), diags).parseModule(sourcePath);
    if (diags.hasErrors() || !astModule) {
        std::string msg = diags.render(buffer);
        if (errOut) *errOut = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    std::optional<types::InferenceResult> inferred;
    if (infer) {
        inferred = types::inferModule(*astModule, diags);
        if (diags.hasErrors() || !inferred) {
            std::string msg = diags.render(buffer);
            if (errOut) *errOut = msg;
            else std::fputs(msg.c_str(), stderr);
            return 1;
        }
    }

    auto ilModule = lower::lowerModule(*astModule, diags,
                                       inferred ? &*inferred : nullptr);
    if (diags.hasErrors() || !ilModule) {
        std::string msg = diags.render(buffer);
        if (errOut) *errOut = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    std::filesystem::path tempObj = std::filesystem::temp_directory_path() /
                                    (std::filesystem::path(sourcePath).stem().string() + "_temp.obj");

    LLVMBackend backend;
    if (!backend.emitObject(*ilModule, tempObj.string(), diags)) {
        std::error_code ec;
        if (std::filesystem::exists(tempObj, ec)) std::filesystem::remove(tempObj, ec);
        std::string msg = diags.render(buffer);
        if (errOut) *errOut = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    bool linked = linkExecutable(tempObj.string(), outputPath, diags);

    std::error_code ec;
    if (std::filesystem::exists(tempObj, ec)) {
        std::filesystem::remove(tempObj, ec);
    }

    if (!linked) {
        std::string msg = diags.hasErrors() ? diags.render(buffer) : "error: linking failed\n";
        if (errOut) *errOut = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    return 0;
#endif
}

int runDriver(int argc, char** argv) {
    if (argc < 2) return fail(kUsage);
    const std::string command = argv[1];

    if (command == "version") {
        std::puts("bronze 0.1.0");
        return 0;
    }

    if (command == "lex" || command == "parse") {
        if (argc < 3) return fail("error: missing <file>\n");
        std::string text;
        if (!readFile(argv[2], text)) return fail(std::string("error: cannot read ") + argv[2] + "\n");
        SourceBuffer buffer(argv[2], std::move(text));
        DiagnosticSink diags;
        auto tokens = Lexer(buffer, diags).lex();
        if (diags.hasErrors()) return fail(diags.render(buffer));

        if (command == "lex") {
            for (const auto& t : tokens) {
                std::printf("%s\t%.*s\n", tokenKindName(t.kind),
                            static_cast<int>(t.text.size()), t.text.data());
            }
            return 0;
        }

        auto module = Parser(std::move(tokens), diags).parseModule(argv[2]);
        if (diags.hasErrors() || !module) return fail(diags.render(buffer));
        std::fputs(ast::dump(*module).c_str(), stdout);
        return 0;
    }

    if (command == "types") {
        if (argc < 3) return fail("error: missing <file>\n");
        return runTypes(argv[2]);
    }

    if (command == "il") {
        if (argc < 3) return fail("error: missing <file>\n");
        std::string sourcePath;
        bool infer = true;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--no-infer") {
                infer = false;
            } else if (sourcePath.empty()) {
                sourcePath = arg;
            } else {
                return fail("error: unexpected argument " + arg + "\n");
            }
        }
        if (sourcePath.empty()) return fail("error: missing <file>\n");
        return runIl(sourcePath, nullptr, infer);
    }

    if (command == "build") {
        if (argc < 3) return fail("error: missing <file>\n");
        std::string sourcePath;
        std::string outputPath = "a.exe";
        bool infer = true;

        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--no-infer") {
                infer = false;
            } else if (arg == "-o") {
                if (i + 1 < argc) {
                    outputPath = argv[++i];
                } else {
                    return fail("error: missing argument for -o\n");
                }
            } else if (sourcePath.empty()) {
                sourcePath = arg;
            } else {
                return fail("error: unexpected argument " + arg + "\n");
            }
        }

        if (sourcePath.empty()) return fail("error: missing <file>\n");
        return runBuild(sourcePath, outputPath, nullptr, infer);
    }

    return fail(kUsage);
}

}  // namespace bronze::cli
