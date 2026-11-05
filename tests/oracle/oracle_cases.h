#pragma once

// The oracle harness's case helpers, shared by the suites in oracle_test.cpp
// and oracle_feedback_test.cpp: finding the cases and their pinned bytes,
// removing a built program, and the worker count.

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifndef TEST_CASES_DIR
#define TEST_CASES_DIR "tests/oracle/cases"
#endif

namespace oracle {

inline bool readFileBytes(const std::filesystem::path& path, std::string& content) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    content.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

// A built program is the executable and the module beside it (cli/link.h);
// removing one and leaving the other is how a stale module gets run by the
// next build's host. The staged runtime stays: it is one file per directory,
// shared by every case built there.
inline void removeProgram(const std::filesystem::path& exe, std::error_code& ec) {
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

inline std::filesystem::path findTestDirectory(const std::filesystem::path& baked,
                                        const std::string& suffix) {
    std::vector<std::filesystem::path> candidates = {
        baked,
        suffix,
        "../" + suffix,
        "../../" + suffix,
        "../../../" + suffix
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

inline std::filesystem::path findCasesDirectory() {
    return findTestDirectory(TEST_CASES_DIR, "tests/oracle/cases");
}

struct OracleCase {
    std::filesystem::path entry;
    std::string id;
};

inline std::vector<OracleCase> casesIn(const std::filesystem::path& dir) {
    std::vector<OracleCase> cases;
    // A case's entry is JavaScript or TypeScript: `<name>.js` / `<name>.ts`,
    // or a directory's `main.js` / `main.ts`. Which grammar a file is read in
    // is its extension's to say, so a TypeScript case is pinned under the
    // name it is written in rather than a `.js` it could never be.
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        const std::filesystem::path ext = entry.path().extension();
        if (entry.is_regular_file() && (ext == ".js" || ext == ".ts")) {
            cases.push_back({entry.path(), entry.path().stem().string()});
            continue;
        }
        if (!entry.is_directory()) continue;
        if (entry.path().filename() == "blocked") continue;
        for (const char* mainName : {"main.js", "main.ts"}) {
            std::filesystem::path main = entry.path() / mainName;
            std::error_code ec;
            if (!std::filesystem::exists(main, ec)) continue;
            cases.push_back({main, entry.path().filename().string()});
            break;
        }
    }
    std::sort(cases.begin(), cases.end(),
              [](const OracleCase& a, const OracleCase& b) { return a.entry < b.entry; });
    return cases;
}

inline unsigned int getWorkerJobCount() {
#ifdef _WIN32
    char* env = nullptr;
    size_t len = 0;
    if (_dupenv_s(&env, &len, "BRONZE_TEST_JOBS") == 0 && env != nullptr) {
        int n = std::atoi(env);
        std::free(env);
        if (n > 0) return static_cast<unsigned int>(n);
    }
#else
    if (const char* env = std::getenv("BRONZE_TEST_JOBS")) {
        int n = std::atoi(env);
        if (n > 0) return static_cast<unsigned int>(n);
    }
#endif
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) return 2;
    // Bounded concurrency: at most 4 threads by default (or hw / 2) to prevent machine saturation
    return std::max(1u, std::min(4u, hw / 2));
}

// Does this case READ THE CLOCK? That is the thing a pinned expectation cannot
// survive, and it is narrower than "mentions Date": `Date.UTC`, `Date.parse`,
// the field constructor and every one of 21.4.4's members are pure functions of
// their arguments, and pinning them is exactly how the Date implementation is
// held to ECMA-262.
//
// The three `Date` clock reads are `Date.now`, `new Date()` and `Date()` — the
// two no-argument constructor forms. A whitespace-tolerant match on the empty
// argument list is what separates them from `new Date(0)`.
//
// `performance.now` is the fourth, and it is a clock this suite cannot pin for
// exactly the same reason: it is monotonic and sub-millisecond, so it is a
// BETTER clock and therefore a worse thing to have in a byte-pinned
// expectation. Its own tests live in tests/cli/performance_now_test.cpp, where
// what is asserted is a relation rather than a string.
//
// The occurrence must be the IDENTIFIER `Date` and not a name that merely ends
// in it: `d.getUTCDate()` is four characters of "Date" followed by an empty
// argument list, and reading it as the constructor would ban the very getters
// these cases exist to pin.
//
// (Local-TIMEZONE dependence is a second hazard this cannot see: a case may
// call `getHours` only inside a relation that holds in every zone. That is a
// rule for the author, stated in tests/oracle/README.md.)
inline bool standsAlone(const std::string& code, size_t at) {
    if (at == 0) return true;
    const char before = code[at - 1];
    // '.' rejects a member access, so a `getUTCDate`/`setDate` suffix and a
    // user's own `foo.Date` are both something other than the global.
    return !(std::isalnum(static_cast<unsigned char>(before)) || before == '_' || before == '$' ||
             before == '.');
}

inline bool readsTheClock(const std::string& code) {
    size_t perf = 0;
    while ((perf = code.find("performance.now", perf)) != std::string::npos) {
        if (standsAlone(code, perf)) return true;
        perf += 11;
    }
    size_t now = 0;
    while ((now = code.find("Date.now", now)) != std::string::npos) {
        if (standsAlone(code, now)) return true;
        now += 4;
    }
    size_t at = 0;
    while ((at = code.find("Date", at)) != std::string::npos) {
        if (!standsAlone(code, at)) {
            at += 4;
            continue;
        }
        size_t after = at + 4;
        while (after < code.size() && (code[after] == ' ' || code[after] == '\t')) ++after;
        if (after < code.size() && code[after] == '(') {
            size_t inner = after + 1;
            while (inner < code.size() &&
                   (code[inner] == ' ' || code[inner] == '\t' || code[inner] == '\n' ||
                    code[inner] == '\r')) {
                ++inner;
            }
            if (inner < code.size() && code[inner] == ')') return true;
        }
        at = after;
    }
    return false;
}

}  // namespace oracle
