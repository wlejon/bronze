#include "il/key.h"

namespace bronze::il {

std::optional<uint32_t> parseIndexKey(std::string_view key) {
    if (key.empty()) return std::nullopt;
    if (key == "0") return 0;
    if (key[0] == '0') return std::nullopt;
    uint64_t val = 0;
    for (char c : key) {
        if (c < '0' || c > '9') return std::nullopt;
        val = val * 10 + static_cast<uint64_t>(c - '0');
        if (val > 4294967294ULL) return std::nullopt;
    }
    return static_cast<uint32_t>(val);
}

}  // namespace bronze::il
