#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <cstdio>

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "cli/driver.h"

#ifndef TEST_CASES_DIR
#define TEST_CASES_DIR "tests/oracle/cases"
#endif

static std::string runAndCaptureOutput(const std::string& command) {
    std::string result;
#ifdef _WIN32
    FILE* pipe = _popen(command.c_str(), "rb");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) return result;
    char buffer[256];
    while (std::size_t bytesRead = std::fread(buffer, 1, sizeof(buffer), pipe)) {
        result.append(buffer, bytesRead);
    }
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return result;
}

static bool readFileContent(const std::filesystem::path& path, std::string& content) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    content.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

static std::filesystem::path findCasesDirectory() {
    std::vector<std::filesystem::path> candidates = {
        TEST_CASES_DIR,
        "tests/oracle/cases",
        "../tests/oracle/cases",
        "../../tests/oracle/cases",
        "../../../tests/oracle/cases"
    };

    std::filesystem::path cwd = std::filesystem::current_path();
    for (const auto& cand : candidates) {
        std::filesystem::path full = cwd / cand;
        std::error_code ec;
        if (std::filesystem::exists(cand, ec) && std::filesystem::is_directory(cand, ec)) {
            return std::filesystem::canonical(cand, ec);
        }
        if (std::filesystem::exists(full, ec) && std::filesystem::is_directory(full, ec)) {
            return std::filesystem::canonical(full, ec);
        }
    }
    return {};
}

TEST_CASE("Node-as-Oracle differential test suite") {
    std::filesystem::path casesDir = findCasesDirectory();
    REQUIRE_MESSAGE(!casesDir.empty(), "Oracle test cases directory not found");

    std::vector<std::filesystem::path> caseFiles;
    for (const auto& entry : std::filesystem::directory_iterator(casesDir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".js") {
            caseFiles.push_back(entry.path());
        }
    }

    REQUIRE_MESSAGE(!caseFiles.empty(), "No .js test cases found in cases directory");

    for (const auto& casePath : caseFiles) {
        SUBCASE(casePath.filename().string().c_str()) {
            std::string code;
            REQUIRE(readFileContent(casePath, code));

            // Ratchet rule: verify non-determinism calls are absent
            CHECK(code.find("Date") == std::string::npos);
            CHECK(code.find("Math.random") == std::string::npos);

            // Run with node
            std::string nodeCmd = "node \"" + casePath.string() + "\"";
            std::string nodeOutput = runAndCaptureOutput(nodeCmd);
            std::string nodeErrMsg = "Node execution returned empty output for " + casePath.string();
            REQUIRE_MESSAGE(!nodeOutput.empty(), nodeErrMsg.c_str());

            // Build executable with bronze
            std::filesystem::path exePath = std::filesystem::temp_directory_path() / (casePath.stem().string() + "_oracle.exe");
            std::error_code ec;
            if (std::filesystem::exists(exePath, ec)) {
                std::filesystem::remove(exePath, ec);
            }

            std::string errOut;
            int status = bronze::cli::runBuild(casePath.string(), exePath.string(), &errOut);
            std::string buildErrMsg = "Bronze build failed for " + casePath.string() + ": " + errOut;
            REQUIRE_MESSAGE(status == 0, buildErrMsg.c_str());
            REQUIRE(std::filesystem::exists(exePath));

            // Run bronze native executable
            std::string bronzeOutput = runAndCaptureOutput("\"" + exePath.string() + "\"");

            // Assert exact byte-for-byte output equality
            CHECK(nodeOutput == bronzeOutput);

            std::filesystem::remove(exePath, ec);
        }
    }
}
