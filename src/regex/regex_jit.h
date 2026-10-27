#pragma once

#include <brass/codegen/grammar_builder.hpp>
#include <brass/codegen/kernel_jit.hpp>
#include <brass/mir/module.hpp>
#include <brass/mir/smt_guard_elim.hpp>
#include <brass/mir/superopt.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "regex/pattern.h"

namespace bronze::regex {

// Signature of the compiled native DFA regex scanner:
//   int64_t regex_dfa_match_at(
//       const int32_t* transition_table,
//       const int32_t* accept_table,
//       int32_t start_state,
//       const uint16_t* units,
//       int64_t length,
//       int64_t start_pos,
//       int64_t* out_match_end);
//
// Returns:
//   1 if a valid match starts at `start_pos` (and writes the match end index into `*out_match_end`),
//   0 if no match starts at `start_pos`,
//  -1 if a non-ASCII code unit (>= 128) is encountered (signals fallback to the full UTF-16 interpreter).
using RegexDfaMatchAtFn = int64_t (*)(
    const int32_t* transition_table,
    const int32_t* accept_table,
    int32_t start_state,
    const uint16_t* units,
    int64_t length,
    int64_t start_pos,
    int64_t* out_match_end);

// Signature of the compiled native full-string DFA search kernel:
//   int64_t regex_dfa_search(
//       const int32_t* transition_table,
//       const int32_t* accept_table,
//       int32_t start_state,
//       const uint16_t* units,
//       int64_t length,
//       int64_t from_pos,
//       int64_t* out_match_start,
//       int64_t* out_match_end);
//
// Returns:
//   1 if a match is found at or after `from_pos` (writes start/end to `out_match_start`/`out_match_end`),
//   0 if no match exists and all scanned units were ASCII (< 128),
//  -1 if a non-ASCII code unit (>= 128) is encountered (signals fallback to the C++ interpreter).
using RegexDfaSearchFn = int64_t (*)(
    const int32_t* transition_table,
    const int32_t* accept_table,
    int32_t start_state,
    const uint16_t* units,
    int64_t length,
    int64_t from_pos,
    int64_t* out_match_start,
    int64_t* out_match_end);

struct RegexJitStats {
    uint32_t nfa_states = 0;
    uint32_t dfa_states = 0;
    uint32_t smt_guards_eliminated = 0;
    uint32_t superopt_masks_elided = 0;
    uint32_t superopt_extensions_elided = 0;
    uint32_t superopt_dags_synthesized = 0;
    uint32_t superopt_solver_queries = 0;
};

// Holds the deterministic state table (256 transitions per state, laid out for
// `brass::codegen::GrammarBuilder::dfa_table_step`) and native JIT-compiled entry points
// for a regular expression without backreferences, lookarounds, or capture groups.
class RegexJitPlan {
public:
    static constexpr uint32_t kMaxNfaStates = 256;
    static constexpr uint32_t kMaxDfaStates = 128;

    RegexJitPlan() = default;

    bool isSupported() const noexcept { return supported_; }
    bool requiresCaptureExtraction() const noexcept { return requires_capture_extraction_; }
    bool anchorsAtStart() const noexcept { return anchors_at_start_; }
    int32_t startState() const noexcept { return start_state_; }
    int32_t numDfaStates() const noexcept { return num_dfa_states_; }

    const std::vector<int32_t>& transitionTable() const noexcept { return transition_table_; }
    const std::vector<int32_t>& acceptTable() const noexcept { return accept_table_; }
    // The plan's own counts, with the shared kernels' proof counts once they
    // are built (zero before).
    RegexJitStats stats() const noexcept;

    // Attempts to execute `matchAt` via the native JIT kernel.
    // Returns true if the JIT kernel answered definitively (filling `status` and `out`),
    // or false if the caller should fall back to the C++ backtracking interpreter.
    bool tryMatchAt(UnitsView input, size_t at, MatchResult& out, ExecStatus& status) const;

    // Attempts to execute `search` via the native JIT kernel.
    // Returns true if the JIT kernel answered definitively (filling `status` and `out`),
    // or false if the caller should fall back to the C++ backtracking interpreter.
    bool trySearch(UnitsView input, size_t from, MatchResult& out, ExecStatus& status) const;

    // Builds the unoptimized `brass::Module` containing the regex DFA kernels
    // (`regex_dfa_step`, `regex_dfa_match_at`, `regex_dfa_search`) using `GrammarBuilder`.
    // Exposed for unit testing and `brass::verify::verify_mir_equivalence`.
    static std::unique_ptr<brass::Module> buildMirModule(
        const int32_t* transition_table = nullptr,
        int32_t num_states = 0);

    // Runs `brass::run_smt_guard_elimination` and `brass::run_mir_superopt` on a regex MIR module.
    static void optimizeMirModule(
        brass::Module& mod,
        brass::SmtGuardElimStats* guard_stats = nullptr,
        brass::SuperoptStats* superopt_stats = nullptr);

    // Compiles a `Pattern` into a `RegexJitPlan` if it uses only regular constructs
    // within state limits; returns nullptr if the pattern requires fallback.
    static std::unique_ptr<RegexJitPlan> tryCompile(const Pattern& pattern);

    // The DFA kernels every plan runs are one generic module, proven and
    // compiled once per process (the solver passes take a few hundred ms). The
    // first plan starts that on a thread of its own and never waits for it:
    // until the kernels are in, a plan answers false from tryMatchAt /
    // trySearch and the interpreter matches, with the same result. This
    // blocks until they are in, for tests that measure the kernels themselves.
    static void waitForKernels();

private:
    bool supported_ = false;
    bool requires_capture_extraction_ = false;
    bool anchors_at_start_ = false;
    int32_t start_state_ = 0;
    int32_t num_dfa_states_ = 0;
    // Flat [num_dfa_states_ * 256] transition table: state * 256 + byte -> next_state (-1 if dead).
    std::vector<int32_t> transition_table_;
    // Flat [num_dfa_states_] accept table:
    //   0 = not accepting
    //   1 = accepting (continue scanning for greedy longest match)
    //   2 = accepting and stop immediately (lazy quantifier / first-alternative priority cut)
    std::vector<int32_t> accept_table_;
    RegexJitStats stats_;
};

// Returns true if `pattern` was compiled with a native `brass` DFA JIT fast path.
bool hasJitFastPath(const Pattern& pattern) noexcept;

}  // namespace bronze::regex
