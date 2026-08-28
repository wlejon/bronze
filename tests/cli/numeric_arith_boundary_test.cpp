// WHO IS ENTITLED TO THE NUMERIC ARITHMETIC ARM.
//
// `*`, `-`, `/` and `%` over an unproven operand have two lowerings: a boxed
// `dynamic` result whose helper owns 13.15.3 in full — including the BigInt
// algorithm, which returns a heap value and therefore earns a GC root slot —
// and an unboxed `f64` result, which is 13.15.3 with the BigInt arm proved
// dead. The second is only sound when no BigInt can reach the operator, and
// that question has two halves: the program's own text, which
// `bigIntMayReach` scans, and whatever crosses a HOST BOUNDARY, which no scan
// can see.
//
// `hasHostBoundary` in src/cli/driver.cpp is the one place that decides
// whether this invocation has such a boundary. These cases pin its three
// answers off `bronze il`, because the IL is where the decision is legible:
// `f64 = mul` or `dynamic = mul`, one instruction, no inference of intent
// required.
//
// The operands have to be genuinely unproven or the test proves nothing: an
// element of a mixed array literal is dynamic no matter how hard inference
// looks, so `vals[i] * vals[j]` is exactly the site the arm is about. A
// program of `1.5 * 2.0` would emit `f64 = mul` under both lowerings and pass
// whatever the driver decided.

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "cli/driver.h"

namespace {

// A mixed array is the cheapest site inference cannot type, and the two reads
// are separate elements so neither can be narrowed from the other.
const char* const kMixedOperands =
    "const vals = [1.5, 'x', {}];\n"
    "function mix(i, j) {\n"
    "  return vals[i] * vals[j];\n"
    "}\n"
    "console.log(mix(0, 0));\n";

// The same program with a BigInt spelled in it. `typeof big` keeps the binding
// live so no elimination pass can decide the literal was never there.
const char* const kMixedOperandsWithBigInt =
    "const vals = [1.5, 'x', {}];\n"
    "const big = 1n;\n"
    "function mix(i, j) {\n"
    "  return vals[i] * vals[j];\n"
    "}\n"
    "console.log(mix(0, 0), typeof big);\n";

std::filesystem::path workDir() {
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "bronze_numeric_arith_boundary";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void write(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

// `source` compiled to IL, optionally against a host-globals manifest and
// optionally with the promise flag.
std::string il(const std::string& name, const char* source, bool withHostGlobals,
               bool assumeNoBigInt) {
    const std::filesystem::path dir = workDir();
    const std::filesystem::path js = dir / (name + ".js");
    write(js, source);
    std::string manifestPath;
    if (withHostGlobals) {
        // Any name at all: what makes this a boundary is that a HOST supplies
        // values here, not which names it supplies.
        const std::filesystem::path manifest = dir / (name + ".globals");
        write(manifest, "document\n");
        manifestPath = manifest.string();
    }
    std::string out;
    const int status = bronze::cli::runIl(js.string(), &out, /*infer=*/true, manifestPath,
                                          /*moduleRoots=*/{}, /*importMapPath=*/{},
                                          /*inferStats=*/false, assumeNoBigInt);
    REQUIRE(status == 0);
    return out;
}

// The A/B seam is read from the environment on every call, so a run with it
// set has one correct answer for every case here and it is the other one.
// Skipping beats inverting: there is no second contract to assert.
bool seamOff() { return std::getenv("BRONZE_NO_NUMERIC_ARITH") != nullptr; }

}  // namespace

TEST_CASE("no host boundary: the numeric arm needs no flag") {
    if (seamOff()) return;
    const std::string out = il("standalone", kMixedOperands, /*withHostGlobals=*/false,
                               /*assumeNoBigInt=*/false);
    CHECK(out.find("f64 = mul") != std::string::npos);
    CHECK(out.find("dynamic = mul") == std::string::npos);

    // And the flag is a no-op there rather than a second, better setting.
    const std::string flagged = il("standalone_flagged", kMixedOperands,
                                   /*withHostGlobals=*/false, /*assumeNoBigInt=*/true);
    CHECK(flagged.find("f64 = mul") != std::string::npos);
    CHECK(flagged.find("dynamic = mul") == std::string::npos);
}

TEST_CASE("a host-globals manifest is a boundary: the arm needs the promise") {
    if (seamOff()) return;
    const std::string bare = il("hosted", kMixedOperands, /*withHostGlobals=*/true,
                                /*assumeNoBigInt=*/false);
    CHECK(bare.find("dynamic = mul") != std::string::npos);
    CHECK(bare.find("f64 = mul") == std::string::npos);

    const std::string promised = il("hosted_promised", kMixedOperands,
                                    /*withHostGlobals=*/true, /*assumeNoBigInt=*/true);
    CHECK(promised.find("f64 = mul") != std::string::npos);
    CHECK(promised.find("dynamic = mul") == std::string::npos);
}

TEST_CASE("a BigInt spelled in the program refuses the arm on either side") {
    if (seamOff()) return;
    // No boundary, so nothing to promise — and the scan still says no.
    const std::string standalone = il("bigint_standalone", kMixedOperandsWithBigInt,
                                      /*withHostGlobals=*/false, /*assumeNoBigInt=*/false);
    CHECK(standalone.find("dynamic = mul") != std::string::npos);
    CHECK(standalone.find("f64 = mul") == std::string::npos);

    // A promise cannot buy what the text refuses: this is the half that makes
    // the flag safe to hand an embedder.
    const std::string promised = il("bigint_promised", kMixedOperandsWithBigInt,
                                    /*withHostGlobals=*/false, /*assumeNoBigInt=*/true);
    CHECK(promised.find("dynamic = mul") != std::string::npos);
    CHECK(promised.find("f64 = mul") == std::string::npos);

    const std::string hosted = il("bigint_hosted", kMixedOperandsWithBigInt,
                                  /*withHostGlobals=*/true, /*assumeNoBigInt=*/true);
    CHECK(hosted.find("dynamic = mul") != std::string::npos);
    CHECK(hosted.find("f64 = mul") == std::string::npos);
}
