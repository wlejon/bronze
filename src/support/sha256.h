#pragma once

#include <string>
#include <string_view>

namespace bronze::support {

// SHA-256 (FIPS 180-4) of `data` as 64 lower-case hex digits. A proof
// certificate names the source it is about with it, so the digest has to be
// the one any other tool computes over the same bytes.
std::string sha256Hex(std::string_view data);

}  // namespace bronze::support
