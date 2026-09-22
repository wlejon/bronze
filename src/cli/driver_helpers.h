#pragma once

#include <string>
#include <vector>
#include <brass/target/target.hpp>
#include "support/diagnostics.h"
#include "support/source.h"
#include "types/pins.h"

namespace bronze::cli {

int fail(const std::string& message);
void reportWarnings(const DiagnosticSink& diags, const SourceSet& sources);
bool readFile(const std::string& path, std::string& out);
bool loadHostGlobals(const std::string& path, std::vector<std::string>& out, std::string& err);
bool loadPins(const std::string& path, types::PinManifest& out, std::string& err, bool allowObserved);
bool hasHostBoundary(const std::string& hostGlobalsPath, bool emitObj, bool emitShared);
bool parseTargetName(const std::string& name, brass::Target& out, std::string& err);

}  // namespace bronze::cli
