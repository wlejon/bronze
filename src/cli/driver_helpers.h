#pragma once

#include <optional>
#include <string>
#include <vector>
#include <brass/object/macho_writer.hpp>
#include <brass/target/target.hpp>
#include "support/diagnostics.h"
#include "support/source.h"
#include "lower/lower_feedback.h"
#include "opt/spec/profile.h"
#include "types/pins.h"

namespace bronze::cli {

int fail(const std::string& message);
void reportWarnings(const DiagnosticSink& diags, const SourceSet& sources);
bool readFile(const std::string& path, std::string& out);
bool loadHostGlobals(const std::string& path, std::vector<std::string>& out, std::string& err);
bool loadPins(const std::string& path, types::PinManifest& out, std::string& err, bool allowObserved);
// `<arch>-<os>`; a Darwin one may name the platform's minimum version and,
// for iOS, the simulator: aarch64-macos13.0, aarch64-ios16.0,
// aarch64-ios16.0-simulator, x64-ios-simulator. iOS is the macOS target of
// that architecture (the same Darwin ABI) with its own Mach-O platform,
// which *machoVersion receives (zero fields: brass's defaults).
bool parseTargetName(const std::string& name, brass::Target& out, std::string& err,
                     brass::object::MachOBuildVersion* machoVersion = nullptr);
// `<name> <value>` or `<name>=<value>`: true when `arg` is that flag (and
// consumes the value), with `err` set when the value is missing.
bool takeFlagValue(const std::string& arg, const char* name, int& i, int argc, char** argv,
                   std::string& out, std::string& err);
// The type-feedback flags (docs/type-feedback.md): `--feedback-out` made
// absolute (the run writes it from wherever it runs) and refused beside
// `--census`, which uses the same runtime channel; `--feedback-in` read and
// parsed into `profile`, which `options` then points at.
bool prepareFeedback(const std::string& feedbackOutPath, const std::string& feedbackInPath,
                     const std::string& censusOutPath, std::optional<spec::TypeProfile>& profile,
                     lower::FeedbackOptions& options, std::string& err);
int dispatchProveEquivArgs(int argc, char** argv);
int dispatchSynthTestsArgs(int argc, char** argv);

}  // namespace bronze::cli
