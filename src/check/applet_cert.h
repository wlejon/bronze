#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "check/static_checker.h"

namespace bronze::check {

enum class HazardKind : uint8_t {
    NullDeref,
    OutOfBounds,
    NaNPoison,
    DivByZero,
};

inline const char* hazardKindName(HazardKind k) {
    switch (k) {
    case HazardKind::NullDeref: return "null-deref";
    case HazardKind::OutOfBounds: return "out-of-bounds";
    case HazardKind::NaNPoison: return "nan-poison";
    case HazardKind::DivByZero: return "div-by-zero";
    }
    return "hazard";
}

struct HazardReport {
    HazardKind kind = HazardKind::NullDeref;
    std::string functionName;
    std::string file;
    uint32_t line = 1;
    uint32_t col = 1;
    Span span;
    std::string message;
};

struct AppletCertOptions {
    std::string soPath;           // Optional compiled .so/.dll/.dylib to hash (defaults to entry.js or <dir>/app.so)
    std::string certOutPath;      // Optional output path for .cert.json (defaults to <dir>/app.cert.json)
    std::string broJsonPath;      // Optional path to bro.json capability manifest
    std::vector<std::string> allowedCapabilities; // Extra allowed capabilities
};

struct AppletCertResult {
    bool verified = false;
    uint32_t hazardCount = 0;
    bool proofsValid = false;
    std::string moduleHash;
    std::vector<std::string> capabilities;
    std::vector<std::string> unauthorizedCapabilities;
    std::vector<HazardReport> hazards;
    std::string certJson;
    std::string certFilePath;
    std::string errorMessage;
};

// Compute 64-bit FNV-1a hash of arbitrary bytes formatted as 16-char lowercase hex.
std::string computeFnv1a64Hex(const void* data, size_t size);
std::string computeFileFnv1a64Hex(const std::string& filePath, bool* okOut = nullptr);

// Run the eBPF-style verified applet gate on `entryOrDirPath` (either a directory
// containing `bro.json` / `main.js` / `index.js` or a `.js` file), verifying:
//   1. Zero static hazards (`NullDeref`, `OutOfBounds`, `DivByZero`, `NaNPoison`)
//   2. Valid IL proof certificates (`il::verifyModuleProofs`)
//   3. Host global & capability bounds against `bro.json`
// And emit the `app.cert.json` sidecar if verification succeeds.
AppletCertResult verifyAndEmitAppletCert(
    const std::string& entryOrDirPath,
    const AppletCertOptions& opts = {});

// CLI entry point for `bronze verify-applet <appdir_or_entry.js> [--so <app.so>] [-o <app.cert.json>]`
int runVerifyAppletCli(int argc, char** argv, std::string* outString = nullptr);

}  // namespace bronze::check
