#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace bronze::il {

// Is this key-pool string a canonical array index (6.1.7)? Leading zeros and
// anything past 2^32-2 are not, so `a["01"]` and `a["4294967295"]` take the
// named-property path both the read and the write send them down.
//
// It lives in `il` so that every consumer (the backend's receiver proofs, the
// whole-program analysis) agrees about it EXACTLY.
std::optional<uint32_t> parseIndexKey(std::string_view key);

}  // namespace bronze::il
