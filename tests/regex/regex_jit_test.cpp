#include <doctest/doctest.h>

#include <brass/verify/smt_equiv.hpp>

#include <chrono>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "pattern_fixture.h"
#include "regex/pattern.h"
#include "regex/regex.h"
#include "regex/regex_jit.h"

using namespace bronze;
using namespace bronze::regex_fixture;

namespace {

struct CompiledPair {
    regex::PatternPtr with_jit;
    regex::PatternPtr interp_only;
};

CompiledPair compilePair(std::string_view source, std::string_view flags = "") {
    // The kernels build off-thread; these tests compare them, so they wait.
    regex::RegexJitPlan::waitForKernels();
    CompiledPair pair;
    pair.with_jit = compileOk(source, flags);
    pair.interp_only = compileOk(source, flags);
    pair.interp_only->jitPlan.reset();
    return pair;
}

void checkDifferentialEquivalence(
    const CompiledPair& pair,
    const regex::Units& input,
    std::string_view pattern_desc) {
    // 1. Compare `search` at every possible starting offset `from` in `[0, input.size()]`
    for (size_t from = 0; from <= input.size(); ++from) {
        regex::MatchResult jit_res;
        regex::MatchResult interp_res;
        std::string jit_err;
        std::string interp_err;

        const auto jit_status = regex::search(*pair.with_jit, input, from, jit_res, jit_err);
        const auto interp_status = regex::search(*pair.interp_only, input, from, interp_res, interp_err);

        INFO("pattern: ", pattern_desc, " from=", from, " input_len=", input.size());
        REQUIRE(jit_status == interp_status);
        if (jit_status == regex::ExecStatus::Match) {
            CHECK(jit_res.start() == interp_res.start());
            CHECK(jit_res.end() == interp_res.end());
            CHECK(jit_res.captures == interp_res.captures);
        }
    }

    // 2. Compare `matchAt` at every possible starting offset `at` in `[0, input.size()]`
    for (size_t at = 0; at <= input.size(); ++at) {
        regex::MatchResult jit_res;
        regex::MatchResult interp_res;
        std::string jit_err;
        std::string interp_err;

        const auto jit_status = regex::matchAt(*pair.with_jit, input, at, jit_res, jit_err);
        const auto interp_status = regex::matchAt(*pair.interp_only, input, at, interp_res, interp_err);

        INFO("pattern: ", pattern_desc, " at=", at, " input_len=", input.size());
        REQUIRE(jit_status == interp_status);
        if (jit_status == regex::ExecStatus::Match) {
            CHECK(jit_res.start() == interp_res.start());
            CHECK(jit_res.end() == interp_res.end());
            CHECK(jit_res.captures == interp_res.captures);
        }
    }
}

}  // namespace

TEST_CASE("brass regex DFA JIT: verify_mir_equivalence proves Unopt MIR == Superopt MIR") {
    auto pat = compileOk("[a-zA-Z_][a-zA-Z0-9_]*|[0-9]+", "");
    REQUIRE(regex::hasJitFastPath(*pat));
    REQUIRE(pat->jitPlan != nullptr);

    regex::RegexJitPlan::waitForKernels();
    const auto stats = pat->jitPlan->stats();
    CHECK(stats.nfa_states >= 4);
    CHECK(stats.dfa_states >= 3);
    // The shared kernels run the SMT passes only under BRONZE_SMT_OPT=1
    // (regex_jit.cpp); what they elide is proven on opt_mod below.

    auto unopt_mod = regex::RegexJitPlan::buildMirModule(
        pat->jitPlan->transitionTable().data(),
        pat->jitPlan->numDfaStates());
    auto opt_mod = regex::RegexJitPlan::buildMirModule(
        pat->jitPlan->transitionTable().data(),
        pat->jitPlan->numDfaStates());

    brass::SmtGuardElimStats guard_stats{};
    brass::SuperoptStats superopt_stats{};
    regex::RegexJitPlan::optimizeMirModule(*opt_mod, &guard_stats, &superopt_stats);

    CHECK(superopt_stats.masks_elided >= 1);

    const char* proven_fn_names[] = {
        "regex_dfa_table_step",
        "regex_dfa_step_specialized",
        "regex_dfa_utf16_step",
        "regex_dfa_scan_bytes",
    };

    brass::verify::EquivOptions eq_opts;
    eq_opts.loop_unroll_bound = 0;

    for (const char* name : proven_fn_names) {
        const brass::Function* f_unopt = unopt_mod->get_function(name);
        const brass::Function* f_opt = opt_mod->get_function(name);
        REQUIRE(f_unopt != nullptr);
        REQUIRE(f_opt != nullptr);

        auto eq = brass::verify::verify_mir_equivalence(*f_unopt, *f_opt, eq_opts);
        INFO("Function: ", std::string(name), " diagnostic: ", eq.diagnostic);
        CHECK(eq.status == brass::verify::EquivStatus::ProvenEquivalent);
    }

    const char* full_loop_fn_names[] = {
        "regex_dfa_match_at",
        "regex_dfa_search",
    };

    for (const char* name : full_loop_fn_names) {
        const brass::Function* f_unopt = unopt_mod->get_function(name);
        const brass::Function* f_opt = opt_mod->get_function(name);
        REQUIRE(f_unopt != nullptr);
        REQUIRE(f_opt != nullptr);

        auto eq = brass::verify::verify_mir_equivalence(*f_unopt, *f_opt, eq_opts);
        INFO("Function: ", std::string(name), " diagnostic: ", eq.diagnostic);
        CHECK_FALSE(eq.is_counterexample());
        CHECK(eq.status != brass::verify::EquivStatus::Unsupported);
    }
}

TEST_CASE("brass regex DFA JIT: differential equivalence across patterns and random corpus") {
    struct PatternSpec {
        const char* pattern;
        const char* flags;
        bool expect_jit;
    };

    const PatternSpec specs[] = {
        {"abc", "", true},
        {"a|ab", "", true},
        {"ab|a", "", true},
        {"a*b+", "", true},
        {"a*?b+?", "", true},
        {"a?b{2,4}c*", "", true},
        {"a{2,4}?b", "", true},
        {"[a-z0-9_]+", "", true},
        {"[^0-9\\s]+", "", true},
        {"\\d+\\.\\d+", "", true},
        {"\\w+@\\w+\\.\\w+", "", true},
        {"foo|bar|baz", "i", true},
        {"[a-f]+", "i", true},
        {"a.*b", "", true},
        {"a.*?b", "", true},
        {"a.*b", "s", true},
        {"^hello[0-9]*", "", true},
        {"(?:foo|bar){1,3}baz?", "", true},
        {"[a-z]+", "u", true},
        {"(a+)b", "", true},
        {"([a-z]+)_([0-9]+)", "", true},
        {"(foo|bar){1,3}(baz)?", "i", true},
        {"^hello([0-9]*)", "", true},
        // Patterns that must cleanly fall back to the backtracking interpreter:
        {"(a)\\1", "", false},
        {"a(?=b)", "", false},
        {"(?<=a)b", "", false},
        {"\\bword\\b", "", false},
        {"end$", "", false},
        {"^line", "m", false},
    };

    std::vector<regex::Units> corpus = {
        u(""),
        u("a"),
        u("ab"),
        u("abc"),
        u("aaaaabbbbb"),
        u("xabcyabcz"),
        u("FOObarBAZ"),
        u("hello12345world"),
        u("123.456 and 78.9"),
        u("user_1@example.org"),
        u("a\nb"),
        u("axxxbxxxb"),
        u("foofoofoobaz"),
        // Non-ASCII UTF-16 inputs (exercise automatic runtime bailout to interpreter):
        unitsOf({'a', 'b', 0x00E9, 'c'}),
        unitsOf({0x03B1, 'f', 'o', 'o'}),
        textOf({'h', 'e', 'l', 'l', 'o', 0x1F600, '1', '2', '3'}),
    };

    // Add 100 deterministic pseudo-random ASCII strings of varying lengths
    std::mt19937 rng(0xB80A55u);
    const std::string alphabet = "abcxyzFOOBAR01239_.@\n ";
    for (int i = 0; i < 100; ++i) {
        const size_t len = static_cast<size_t>(i % 24);
        std::string s;
        s.reserve(len);
        for (size_t j = 0; j < len; ++j) {
            s.push_back(alphabet[rng() % alphabet.size()]);
        }
        corpus.push_back(u(s));
    }

    for (const auto& spec : specs) {
        CompiledPair pair = compilePair(spec.pattern, spec.flags);
        INFO("Pattern /", spec.pattern, "/", spec.flags);
        CHECK(regex::hasJitFastPath(*pair.with_jit) == spec.expect_jit);
        CHECK_FALSE(regex::hasJitFastPath(*pair.interp_only));

        for (const auto& input : corpus) {
            checkDifferentialEquivalence(pair, input, spec.pattern);
        }
    }
}

TEST_CASE("brass regex DFA JIT: throughput benchmark vs backtracking interpreter") {
    CompiledPair pair = compilePair("(?:[a-z]{2,6}_[0-9]{2,4}|foo[0-9]+bar|baz_qux_[a-z]+)", "");
    REQUIRE(regex::hasJitFastPath(*pair.with_jit));

    // Construct a 4096-unit ASCII subject where matches occur near the end
    std::string text;
    text.reserve(4096);
    for (int i = 0; i < 250; ++i) {
        text += "abc_9x_nope_12345_";
    }
    text += "foo987654bar_tail";
    const regex::Units subject = u(text);

    constexpr int kIterations = 250;
    int64_t jit_checksum = 0;
    int64_t interp_checksum = 0;

    const auto t0 = std::chrono::steady_clock::now();
    for (int iter = 0; iter < kIterations; ++iter) {
        regex::MatchResult res;
        std::string err;
        auto st = regex::search(*pair.with_jit, subject, 0, res, err);
        if (st == regex::ExecStatus::Match) {
            jit_checksum += res.start() + res.end();
        }
    }
    const auto t1 = std::chrono::steady_clock::now();

    for (int iter = 0; iter < kIterations; ++iter) {
        regex::MatchResult res;
        std::string err;
        auto st = regex::search(*pair.interp_only, subject, 0, res, err);
        if (st == regex::ExecStatus::Match) {
            interp_checksum += res.start() + res.end();
        }
    }
    const auto t2 = std::chrono::steady_clock::now();

    REQUIRE(jit_checksum == interp_checksum);
    REQUIRE(jit_checksum > 0);

    const double jit_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    const double interp_us = std::chrono::duration<double, std::micro>(t2 - t1).count();
    const double speedup = (jit_us > 0.0) ? (interp_us / jit_us) : 1.0;

    std::cout << "[RegexJitBenchmark] brass DFA JIT: " << jit_us << " us vs Interpreter: "
              << interp_us << " us (speedup: " << speedup << "x)\n";
}
