#pragma once

#include <optional>
#include <string>

#include "lower/native_manifest.h"

namespace bronze::cli {

std::optional<lower::NativeManifest> resolveNativeManifest(
    const std::string& manifestPath,
    const std::string& libPath,
    std::string& err);

}  // namespace bronze::cli
