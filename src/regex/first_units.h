#pragma once

#include <cstdint>

// The code units a match of a pattern can BEGIN with, when that is a set
// worth knowing: `search` (22.2.7.2 step 12) tries the matcher at every index
// of the input in turn, and an index whose unit is not in this set is one the
// matcher would refuse after setting itself up. `/(["'])[^'"]+\1/` begins with
// a quote and nothing else, so a search of "sans-serif" starts the matcher
// nowhere.

namespace bronze::regex {

struct Node;
struct Flags;

struct FirstUnits {
    // False when the set says nothing: the pattern can match the empty
    // string (so every index is a candidate, the one past the end included),
    // or it is read under `i`, `u` or `v`, where a unit of the input is not
    // what a Char or a Class of the tree is compared with.
    bool usable = false;
    // Units 0..127, one bit each, and whether any unit above them can begin
    // a match.
    uint64_t ascii[2] = {0, 0};
    bool aboveAscii = false;

    bool admits(char16_t unit) const noexcept {
        if (unit >= 128) return aboveAscii;
        return (ascii[unit >> 6] >> (unit & 63)) & 1;
    }
};

FirstUnits computeFirstUnits(const Node& root, const Flags& flags);

}  // namespace bronze::regex
