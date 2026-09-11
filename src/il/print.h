#pragma once
#include <string>

#include "il/il.h"

#include <vector>

namespace bronze::il {

// Canonical text form. Deterministic by construction: iteration order is
// storage order, floats print via a fixed round-trippable format, no locale
// anywhere. The differential ratchets compare this output byte-for-byte.
std::string print(const Module& module, const std::vector<std::string>& fnNames = {});

}  // namespace bronze::il
