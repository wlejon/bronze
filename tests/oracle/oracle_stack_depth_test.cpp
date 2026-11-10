// BRONZE_GC_STRESS changes when collections run, never how deep a program
// may recurse. Every collection under stress walks the whole stack, so a run
// that recurses until RangeError costs the square of its depth, and a
// stress-only cut of the stack span (rtStackLimitFor) once made stressed runs
// cheaper that way: it left a third of the stack, and tiers_06_ackermann
// (about 4100 nested calls) and async_suspend_protocol's depth(800) threw
// RangeError at tier 0 under stress while passing everywhere else. A stressed
// run must reach about as deep as a plain one at each fixed tier.

#include <filesystem>
#include <fstream>
#include <string>

#include <doctest/doctest.h>

#include "../test_temp_dir.h"
#include "run_process.h"

namespace {

// The deepest call a recursion reached before the RangeError, or -1 when the
// run printed something else.
long recursionDepth(const std::filesystem::path& js, const char* tier, bool gcStress) {
    const oracle::RunResult run = oracle::runCommand(
        oracle::quoted(TEST_BRONZE_CLI) + " run --tier=" + tier + " " + oracle::quoted(js.string()), gcStress);
    const std::string prefix = "depth ";
    if (!run.ran || run.exitCode != 0 || run.output.rfind(prefix, 0) != 0) return -1;
    return std::stol(run.output.substr(prefix.size()));
}

}  // namespace

TEST_CASE("gc stress leaves a recursion the depth it has without it, at each tier") {
    const std::filesystem::path js = bronze_test::tempDir() / "gc_stress_recursion_depth.js";
    {
        std::ofstream out(js, std::ios::binary);
        out << "let max = 0;\n"
               "function d(n) { max = n; return d(n + 1) + 1; }\n"
               "try { d(0); } catch (e) { console.log('depth ' + max); }\n";
    }
    // Not auto: there the depth is wherever a background tier-up lands
    // mid-recursion, which differs between any two runs.
    for (const char* tier : {"0", "1", "2"}) {
        const long plain = recursionDepth(js, tier, /*gcStress=*/false);
        const long stressed = recursionDepth(js, tier, /*gcStress=*/true);
        const std::string where = std::string("--tier=") + tier + ": plain " + std::to_string(plain) +
                                  ", gc-stress " + std::to_string(stressed);
        REQUIRE_MESSAGE(plain > 0, where);
        REQUIRE_MESSAGE(stressed > 0, where);
        // Not equal: a collection at the deepest allocation needs its own
        // frames below the last call, and the depths move by a few frames
        // between runs. A quarter less is far inside what a lowered limit or
        // a bigger stressed frame costs (the cut above left 30%).
        CHECK_MESSAGE(stressed * 4 >= plain * 3, where);
    }
}

// Async recursion past the stack: the innermost call's RangeError rejects
// every level above it, and the program goes on. Plain runs only: under
// gc-stress a run to the stack's end costs the square of its depth (76 s for
// this one on a fast machine, past the limit on a slow one), which is why it
// is here and not in an oracle case every stressed suite runs.
TEST_CASE("async recursion past the stack rejects with a RangeError at every tier") {
    const std::filesystem::path js = bronze_test::tempDir() / "async_recursion_past_stack.js";
    {
        std::ofstream out(js, std::ios::binary);
        out << "async function depth(n) { if (n === 0) return 0; const below = await depth(n - 1); return below + 1; }\n"
               "(async () => {\n"
               "  try { await depth(100000); console.log('no throw'); } catch (e) { console.log('too deep: ' + e.name); }\n"
               "  console.log('after: ' + await depth(50));\n"
               "})();\n";
    }
    for (const char* tier : {"0", "1", "2"}) {
        const oracle::RunResult run = oracle::runCommand(
            oracle::quoted(TEST_BRONZE_CLI) + " run --tier=" + tier + " " + oracle::quoted(js.string()),
            /*gcStress=*/false);
        const std::string where = std::string("--tier=") + tier + ": " + run.output + run.errors;
        CHECK_MESSAGE(run.ran, where);
        CHECK_MESSAGE(run.output == "too deep: RangeError\nafter: 50\n", where);
    }
}
