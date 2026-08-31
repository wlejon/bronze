// LINK ORDER IS AN INPUT, AND CHANGING IT CHANGES NOTHING ABOUT THE PROGRAM.
//
// `--link-seed` exists so a measurement can move binary layout on purpose
// (link_order.h says why). That is only useful if it moves layout and NOTHING
// else — a knob that also perturbs behaviour would make every number taken
// under it unreadable — so what is asserted here is the other side of the
// seam: the same objects, permuted, still link into a program that prints the
// same bytes, and with no seed set the order handed to the linker is the order
// the backend emitted, character for character.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cli/driver.h"
#include "cli/link_order.h"

namespace {

std::vector<std::string> sixObjects() {
    return {"a.p0.obj", "a.p1.obj", "a.p2.obj", "a.p3.obj", "a.p4.obj", "a.p5.obj"};
}

// The settings are process-wide, so a test that sets one puts it back — the
// rest of this binary compiles and links programs too.
struct SeedGuard {
    ~SeedGuard() {
        bronze::cli::setLinkSeed(std::nullopt);
        bronze::cli::setKeptObjectDir("");
    }
};

}  // namespace

TEST_CASE("no seed is the order the backend emitted") {
    SeedGuard guard;
    bronze::cli::setLinkSeed(std::nullopt);
    const std::vector<std::string> objs = sixObjects();
    CHECK(bronze::cli::orderForLink(objs) == objs);
}

TEST_CASE("a seeded order is a permutation, and the same one every time") {
    SeedGuard guard;
    const std::vector<std::string> objs = sixObjects();

    bronze::cli::setLinkSeed(uint64_t{7});
    const std::vector<std::string> seven = bronze::cli::orderForLink(objs);
    CHECK(seven.size() == objs.size());

    // Same files, no duplicates, none dropped: the linker gets every object
    // exactly once or the program it produces is a different one.
    std::vector<std::string> sortedSeven = seven;
    std::sort(sortedSeven.begin(), sortedSeven.end());
    CHECK(sortedSeven == objs);

    // Deterministic: a spread reported across seeds is only meaningful if a
    // seed names one fixed layout rather than one draw of a distribution.
    CHECK(bronze::cli::orderForLink(objs) == seven);

    bronze::cli::setLinkSeed(uint64_t{7});
    CHECK(bronze::cli::orderForLink(objs) == seven);

    // And it is a knob: some other seed reaches a different order, or the
    // sweep it is there to drive would measure one layout six times.
    bool anyDiffers = false;
    for (uint64_t seed = 1; seed <= 8 && !anyDiffers; ++seed) {
        if (seed == 7) continue;
        bronze::cli::setLinkSeed(seed);
        anyDiffers = bronze::cli::orderForLink(objs) != seven;
    }
    CHECK(anyDiffers);
}

TEST_CASE("a single object has no order to permute") {
    SeedGuard guard;
    bronze::cli::setLinkSeed(uint64_t{12345});
    const std::vector<std::string> one = {"only.obj"};
    CHECK(bronze::cli::orderForLink(one) == one);
}

#if BRONZE_WITH_LLVM

TEST_CASE("two seeds link the same objects into the same program") {
    SeedGuard guard;

    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "bronze_link_order_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    const std::filesystem::path js = dir / "prog.js";
    {
        std::ofstream out(js, std::ios::binary);
        out << "let total = 0;\n"
               "for (let i = 0; i < 1000; i++) total += i * 3;\n"
               "console.log('total=' + total);\n"
               "console.log([1, 2, 3].map(function (v) { return v * 2; }).join(','));\n";
    }

    const std::filesystem::path objDir = dir / "objs";
    bronze::cli::setKeptObjectDir(objDir.string());

    std::string err;
    const std::filesystem::path built = dir / "built.exe";
    REQUIRE_MESSAGE(bronze::cli::runBuild(js.string(), built.string(), &err) == 0, err);
    bronze::cli::setKeptObjectDir("");

    // The objects outlived the build that emitted them: that is what makes a
    // per-seed link cost a link rather than a compile.
    size_t objCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator(objDir, ec)) {
        const std::string ext = entry.path().extension().string();
        if (ext == ".obj" || ext == ".o") ++objCount;
    }
    REQUIRE(objCount >= 1);

    auto linkUnder = [&](std::optional<uint64_t> seed, const char* name) -> std::string {
        bronze::cli::setLinkSeed(seed);
        const std::filesystem::path exe = dir / (std::string(name) + ".exe");
        std::string linkErr;
        REQUIRE_MESSAGE(
            bronze::cli::linkFromObjectDir(objDir.string(), exe.string(), &linkErr) == 0, linkErr);
#ifdef _WIN32
        FILE* pipe = _popen(exe.string().c_str(), "r");
#else
        FILE* pipe = popen(exe.string().c_str(), "r");
#endif
        REQUIRE(pipe != nullptr);
        std::string out;
        char buffer[256];
        while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) out += buffer;
#ifdef _WIN32
        _pclose(pipe);
#else
        pclose(pipe);
#endif
        return out;
    };

    const std::string unseeded = linkUnder(std::nullopt, "unseeded");
    REQUIRE(!unseeded.empty());
    CHECK(linkUnder(uint64_t{0x5eedA11Cull}, "seed_a") == unseeded);
    CHECK(linkUnder(uint64_t{0xB0BB1E5ull}, "seed_b") == unseeded);
}

#endif  // BRONZE_WITH_LLVM
