// COMPILATION IS A FUNCTION OF ITS INPUT.
//
// Not a style rule: `--no-infer` is only a bisection seam if the two sides are
// each reproducible, an `.expected` file is only a ratchet if the bytes it
// pins are the bytes every run produces, and an A/B of two builds is only a
// measurement if the difference between them is the change under test. A
// compiler that answers differently on two runs of the same input breaks all
// three at once, and it breaks them intermittently, which is worse.
//
// The way that property is lost here is an ADDRESS. Lowering carries proofs
// keyed by AST node pointer, and a body's nodes are not always the module
// tree's: a class constructor is lowered from a copy that dies with
// `lowerClass`. A proof outliving such a copy is answered for whatever node
// the allocator puts at that address next — which under ASLR is a different
// node on different runs. So these two tests are the same assertion at two
// scales: the smallest program that can show the collision, and a real
// library-sized graph.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cli/driver.h"

#ifndef TEST_THREEJS_DIR
#define TEST_THREEJS_DIR "tests/oracle/threejs"
#endif

namespace {

// Two constructors whose bodies differ only in the literals their nested
// declaration is called with. `PickNum`'s copy is freed before `PickStr`'s is
// built, so the second lands on the first's addresses — which is what turns a
// proof that outlived its body into an f64 slot for a parameter every call site
// passes a string to. The `.expected` half of this is
// `tests/oracle/cases/closure_param_ctor_clone.js`; here the IL is asserted
// directly, because the IL is where the wrong answer is legible rather than
// merely visible as a NaN.
const char* const kCtorClonePair =
    "class PickNum {\n"
    "  constructor() {\n"
    "    function pick(v, k) { return v; }\n"
    "    this.a = pick(1, 2);\n"
    "    this.b = pick(3, 4);\n"
    "  }\n"
    "}\n"
    "class PickStr {\n"
    "  constructor() {\n"
    "    function pick(v, k) { return v; }\n"
    "    this.a = pick('m', 'n');\n"
    "    this.b = pick('o', 'p');\n"
    "  }\n"
    "}\n"
    "const pn = new PickNum();\n"
    "const ps = new PickStr();\n"
    "console.log(pn.a, pn.b, ps.a, ps.b);\n";

std::filesystem::path workDir() {
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "bronze_lowering_determinism";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::filesystem::path writeSource(const std::string& name, const char* source) {
    const std::filesystem::path js = workDir() / (name + ".js");
    std::ofstream out(js, std::ios::binary);
    out << source;
    return js;
}

std::filesystem::path findThreejsMain() {
    const std::vector<std::filesystem::path> candidates = {
        std::filesystem::path(TEST_THREEJS_DIR),
        "tests/oracle/threejs",
        "../tests/oracle/threejs",
        "../../tests/oracle/threejs",
        "../../../tests/oracle/threejs",
    };
    for (const auto& cand : candidates) {
        std::error_code ec;
        const std::filesystem::path main = cand / "main.js";
        if (std::filesystem::exists(main, ec)) return std::filesystem::canonical(main, ec);
    }
    return {};
}

// The IL for one function's signature line, so a failure names the parameter
// that moved rather than diffing tens of thousands of lines.
std::vector<std::string> signatureLines(const std::string& il, const std::string& fnName) {
    std::vector<std::string> out;
    const std::string prefix = "func " + fnName + "(";
    size_t pos = 0;
    while (pos < il.size()) {
        const size_t eol = il.find('\n', pos);
        const std::string line = il.substr(pos, eol == std::string::npos ? eol : eol - pos);
        if (line.compare(0, prefix.size(), prefix) == 0) out.push_back(line);
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return out;
}

}  // namespace

TEST_CASE("a constructor's copied body cannot lend its proofs to the next one") {
    const std::filesystem::path js = writeSource("ctor_clone_pair", kCtorClonePair);

    std::string il;
    REQUIRE(bronze::cli::runIl(js.string(), &il) == 0);

    const std::vector<std::string> picks = signatureLines(il, "pick");
    REQUIRE(picks.size() == 2);
    // Every site of the first passes a Number, so it earns the f64 slots.
    CHECK(picks[0] == "func pick(%0: f64, %1: f64) -> dynamic {");
    // Every site of the second passes a string. Nothing about the first is
    // evidence about the second, however the allocator seats them.
    CHECK(picks[1] == "func pick(%0: dynamic, %1: dynamic) -> dynamic {");

    // And the whole answer is the same one every time it is asked for. Three
    // lowerings in one process do not vary the addresses the way separate
    // processes do, but they do hand each run a differently fragmented heap,
    // which is the same hazard from the other side.
    for (int i = 0; i < 2; ++i) {
        std::string again;
        REQUIRE(bronze::cli::runIl(js.string(), &again) == 0);
        CHECK(again == il);
    }
}

TEST_CASE("the three.js graph lowers to the same IL every time") {
    const std::filesystem::path main = findThreejsMain();
    REQUIRE_MESSAGE(!main.empty(), "tests/oracle/threejs/main.js not found");

    std::string first;
    REQUIRE(bronze::cli::runIl(main.string(), &first) == 0);
    REQUIRE(!first.empty());

    for (int i = 0; i < 2; ++i) {
        std::string again;
        REQUIRE(bronze::cli::runIl(main.string(), &again) == 0);
        // Byte-for-byte: a single differing parameter type is a proof that
        // depended on where the allocator put a node, and the oracle's pinned
        // expectations are decided by exactly these bytes.
        CHECK(again.size() == first.size());
        CHECK(again == first);
    }
}
