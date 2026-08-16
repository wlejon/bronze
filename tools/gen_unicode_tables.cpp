// tools/gen_unicode_tables.cpp
//
// Generates the Unicode data tables bronze carries, from the published UCD
// 16.0.0 data files vendored under tools/ucd/. Two modules are served and they
// need different data, so the output goes to both:
//
//   src/regex/    General_Category runs, simple case FOLDING (22.2.2.9's
//                 Canonicalize under `u` and `i`), and the Script /
//                 Script_Extensions tables of UAX #24.
//   src/runtime/  DEFAULT CASE CONVERSION -- the simple and full case mappings
//                 String.prototype.toUpperCase / toLowerCase apply (11.1.3),
//                 plus the two derived properties the Final_Sigma condition is
//                 written in terms of.
//
// Folding and conversion are different operations over different tables, and
// each module's half is its own translation unit for that reason:
// `scf("ẞ")` is "ß" where `toLowerCase("ẞ")` is also "ß" but `toUpperCase("ß")`
// is "SS", which no folding table can spell. This file is the composition root
// and nothing else -- it finds the inputs and calls the two halves.
//
// Run once, commit the output. The build never invokes this: bronze does not
// depend on external generator tooling at build time, and a table regenerated
// during build cannot be reviewed or audited.
//
// Usage:
//   gen_unicode_tables [ucd_dir] [src_dir]

#include <filesystem>
#include <iostream>
#include <vector>

#include "gen_unicode_common.h"

int main(int argc, char** argv) {
    std::filesystem::path ucdDir;
    std::filesystem::path outDir;

    if (argc >= 3) {
        ucdDir = argv[1];
        outDir = argv[2];
    } else {
        // Auto-locate relative to current working directory
        std::vector<std::filesystem::path> searchBases = {
            ".",
            "..",
            "../..",
            "../../..",
        };
        for (const auto& base : searchBases) {
            if (std::filesystem::exists(base / "tools/ucd/UnicodeData.txt") &&
                std::filesystem::exists(base / "src/regex/unicode_data.h")) {
                ucdDir = base / "tools/ucd";
                // The SOURCE root, not one module's directory: two modules are
                // served and each gets the tables its own operation needs.
                outDir = base / "src";
                break;
            }
        }
    }

    const char* const kInputs[] = {"UnicodeData.txt",   "CaseFolding.txt",
                                   "SpecialCasing.txt", "DerivedCoreProperties.txt",
                                   "Scripts.txt",       "ScriptExtensions.txt",
                                   "PropertyValueAliases.txt"};
    bool haveInputs = !ucdDir.empty() && !outDir.empty();
    for (const char* name : kInputs) {
        if (!haveInputs) break;
        haveInputs = std::filesystem::exists(ucdDir / name);
        if (!haveInputs) std::cerr << "error: missing " << (ucdDir / name).string() << "\n";
    }
    if (!haveInputs) {
        std::cerr << "error: could not locate UCD data files under " << ucdDir << "\n";
        std::cerr << "Usage: gen_unicode_tables [ucd_dir] [src_dir]\n";
        return 1;
    }

    std::cout << "Reading UCD " << gen::kUcdVersion << " from " << ucdDir.string() << "...\n";

    gen::generateRegexTables(ucdDir, outDir);
    gen::generateRuntimeTables(ucdDir, outDir);

    std::cout << "Done.\n";
    return 0;
}
