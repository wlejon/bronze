// WHEN A BUILD GETS THE NUMERIC ARITHMETIC ARM.
//
// `*`, `-`, `/` and `%` over an unproven operand have two lowerings: a boxed
// `dynamic` result whose helper owns 13.15.3 in full, including the BigInt
// algorithm, which returns a heap value and therefore earns a GC root slot,
// and an unboxed `f64` result, which is 13.15.3 with the BigInt arm dead.
// Which one an ahead-of-time build gets is `bigIntMayReach`'s answer over the
// program's own text and nothing else: a host boundary (a `--host-globals`
// manifest) does not change it. These cases pin that off `bronze il`, because
// the IL is where the decision is legible: `f64 = mul` or `dynamic = mul`,
// one instruction.
//
// The operands have to be genuinely unproven or the test proves nothing: an
// element of a mixed array literal is dynamic no matter how hard inference
// looks, so `vals[i] * vals[j]` is exactly the site the arm is about. A
// program of `1.5 * 2.0` would emit `f64 = mul` under both lowerings.

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "../test_temp_dir.h"
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
        bronze_test::tempDir() / "bronze_numeric_arith_boundary";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void write(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

// `source` compiled to IL, optionally against a host-globals manifest.
std::string il(const std::string& name, const char* source, bool withHostGlobals) {
    const std::filesystem::path dir = workDir();
    const std::filesystem::path js = dir / (name + ".js");
    write(js, source);
    std::string manifestPath;
    if (withHostGlobals) {
        const std::filesystem::path manifest = dir / (name + ".globals");
        write(manifest, "document\n");
        manifestPath = manifest.string();
    }
    std::string out;
    const int status = bronze::cli::runIl(js.string(), &out, /*infer=*/true, manifestPath);
    REQUIRE(status == 0);
    return out;
}

}  // namespace

TEST_CASE("a program that spells no BigInt gets the numeric arm, hosted or not") {
    for (const bool hosted : {false, true}) {
        const std::string out = il(hosted ? "hosted" : "standalone", kMixedOperands, hosted);
        CHECK(out.find("f64 = mul") != std::string::npos);
        CHECK(out.find("dynamic = mul") == std::string::npos);
    }
}

TEST_CASE("a BigInt spelled in the program refuses the arm, hosted or not") {
    for (const bool hosted : {false, true}) {
        const std::string out =
            il(hosted ? "bigint_hosted" : "bigint_standalone", kMixedOperandsWithBigInt, hosted);
        CHECK(out.find("dynamic = mul") != std::string::npos);
        CHECK(out.find("f64 = mul") == std::string::npos);
    }
}
