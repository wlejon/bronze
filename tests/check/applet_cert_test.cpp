#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "check/applet_cert.h"

namespace fs = std::filesystem;

namespace {

void writeTextFile(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << content;
}

}  // namespace

TEST_CASE("applet_cert: verifies clean applet and emits app.cert.json with FNV-1a hash") {
    fs::path dir = fs::temp_directory_path() / "bronze_applet_cert_clean";
    fs::remove_all(dir);
    fs::create_directories(dir);

    writeTextFile(dir / "bro.json", R"({
  "name": "clean-applet",
  "main": "main.js",
  "verifiedApplet": true,
  "capabilities": ["bro.ui", "bro.canvas"]
})");

    writeTextFile(dir / "main.js", R"(
function computeScore(a, b) {
    return a * 2 + b;
}
const s = computeScore(10, 20);
bro.ui.setTitle(s);
)");

    writeTextFile(dir / "app.so", "FAKE_ELF_BINARY_PAYLOAD_12345");

    auto res = bronze::check::verifyAndEmitAppletCert(dir.string());
    INFO(res.errorMessage);
    REQUIRE(res.verified);
    CHECK(res.hazardCount == 0);
    CHECK(res.proofsValid);
    CHECK(res.unauthorizedCapabilities.empty());
    CHECK(res.moduleHash == bronze::check::computeFileFnv1a64Hex((dir / "app.so").string()));
    CHECK(fs::exists(dir / "app.cert.json"));
    CHECK(res.certJson.find("\"verified\": true") != std::string::npos);
    CHECK(res.certJson.find("\"hazards\": 0") != std::string::npos);
    CHECK(res.certJson.find("\"bro.ui.setTitle\"") != std::string::npos);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("applet_cert: rejects applet with static NullDeref or DivByZero hazard") {
    fs::path dir = fs::temp_directory_path() / "bronze_applet_cert_hazard";
    fs::remove_all(dir);
    fs::create_directories(dir);

    SUBCASE("NullDeref hazard") {
        writeTextFile(dir / "main.js", R"(
function crashNull() {
    let p = null;
    return p.x;
}
crashNull();
)");
        auto res = bronze::check::verifyAndEmitAppletCert(dir.string());
        CHECK_FALSE(res.verified);
        CHECK(res.hazardCount > 0);
        CHECK_FALSE(fs::exists(dir / "app.cert.json"));
    }

    SUBCASE("DivByZero hazard") {
        writeTextFile(dir / "main.js", R"(
function crashDiv(x) {
    return x / 0;
}
crashDiv(42);
)");
        auto res = bronze::check::verifyAndEmitAppletCert(dir.string());
        CHECK_FALSE(res.verified);
        CHECK(res.hazardCount > 0);
        CHECK(res.errorMessage.find("div-by-zero") != std::string::npos);
        CHECK_FALSE(fs::exists(dir / "app.cert.json"));
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("applet_cert: rejects applet using unauthorized capability or child_process") {
    fs::path dir = fs::temp_directory_path() / "bronze_applet_cert_unauth";
    fs::remove_all(dir);
    fs::create_directories(dir);

    writeTextFile(dir / "bro.json", R"({
  "name": "untrusted-applet",
  "main": "main.js",
  "verifiedApplet": true,
  "capabilities": ["bro.ui"]
})");

    writeTextFile(dir / "main.js", R"(
function sneakExec() {
    return child_process.execSync("id");
}
sneakExec();
)");

    auto res = bronze::check::verifyAndEmitAppletCert(dir.string());
    CHECK_FALSE(res.verified);
    CHECK_FALSE(res.unauthorizedCapabilities.empty());
    CHECK(res.errorMessage.find("UnauthorizedCapability") != std::string::npos);
    CHECK(res.errorMessage.find("child_process") != std::string::npos);
    CHECK_FALSE(fs::exists(dir / "app.cert.json"));

    std::error_code ec;
    fs::remove_all(dir, ec);
}
