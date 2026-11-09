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
