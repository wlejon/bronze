#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace bronze::il {

// Is this key-pool string a canonical array index (6.1.7)? Leading zeros and
// anything past 2^32-2 are not, so `a["01"]` and `a["4294967295"]` take the
// named-property path both the read and the write send them down.
//
// It lives in `il` and not beside the inline cache it was written for because
// two passes in two modules now have to agree about it EXACTLY. The backend
// asks it to decide which reads a receiver proof covers; the guarded-region
// pass (src/lower/guard_region.h) asks it to decide which reads one
// `is.dense_array` licenses. A key the region pass counts as an index and the
// backend does not is a read left on the property cache inside a run the region
// pass has already reordered around — which is a getter run at the wrong time,
// not a missed optimisation. One definition is what makes that impossible.
std::optional<uint32_t> parseIndexKey(std::string_view key);

}  // namespace bronze::il
