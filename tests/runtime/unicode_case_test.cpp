// Default Case Conversion, tested at the seam below the String members: units
// in, units out, with no heap and no JS. What `builtin_string.cpp` adds on top
// is `thisStringValue` and a string allocation, neither of which can be wrong
// in a way that a case table would explain — so the questions that ARE about
// the tables are asked here, where a failure names the mapping rather than the
// member.
//
// The cases are the ones a simple (1:1) table would get wrong, because a simple
// table is what bronze had before these existed and is the thing this file
// exists to rule out. It replaces nothing: the previous behaviour was a
// `fatal()` on any non-ASCII input, which no test pinned because a fatal aborts
// the process.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "runtime/unicode_case.h"

using namespace bronze::runtime;

namespace {

using Units = std::vector<uint16_t>;

Units upper(const Units& in) { return unicode::toUpperFull(in); }
Units lower(const Units& in) { return unicode::toLowerFull(in); }

}  // namespace

TEST_CASE("ASCII case conversion is unchanged by the tables") {
    const Units mixed = {'a', 'B', 'c', ' ', '1', '!', 'Z'};
    CHECK(upper(mixed) == Units{'A', 'B', 'C', ' ', '1', '!', 'Z'});
    CHECK(lower(mixed) == Units{'a', 'b', 'c', ' ', '1', '!', 'z'});
    CHECK(upper(Units{}) == Units{});
    CHECK(lower(Units{}) == Units{});
}

TEST_CASE("a full case mapping may be longer than its input") {
    // U+00DF uppercases to "SS" (SpecialCasing.txt), and there is no simple
    // uppercase for it at all -- which is why a table built from
    // UnicodeData.txt alone would answer the sharp s unchanged.
    CHECK(upper(Units{0x00DF}) == Units{'S', 'S'});
    // U+FB01 LATIN SMALL LIGATURE FI -> "FI", and U+0149 -> U+02BC U+004E.
    CHECK(upper(Units{0xFB01}) == Units{'F', 'I'});
    CHECK(upper(Units{0x0149}) == Units{0x02BC, 0x004E});
    // U+0130 lowercases to i + COMBINING DOT ABOVE. The bare "i" answer is the
    // Turkish tailoring, which default casing excludes.
    CHECK(lower(Units{0x0130}) == Units{0x0069, 0x0307});
}

TEST_CASE("the sharp s round trip is lossy in one direction only") {
    // upper("ß") is "SS" and lower("SS") is "ss": the round trip does not
    // return. U+1E9E, the capital sharp s, lowercases to U+00DF, so the
    // character has an uppercase it does not come back from.
    CHECK(lower(upper(Units{0x00DF})) == Units{'s', 's'});
    CHECK(lower(Units{0x1E9E}) == Units{0x00DF});
    CHECK(upper(Units{0x1E9E}) == Units{0x1E9E});
}

TEST_CASE("Final_Sigma picks between the two lowercase sigmas by context") {
    constexpr uint16_t kCapital = 0x03A3;
    constexpr uint16_t kFinal = 0x03C2;
    constexpr uint16_t kSmall = 0x03C3;
    constexpr uint16_t kAlpha = 0x0391;
    constexpr uint16_t kSmallAlpha = 0x03B1;

    // No cased letter before it: not final.
    CHECK(lower(Units{kCapital}) == Units{kSmall});
    // "ΣΑΣ" -> "σας".
    CHECK(lower(Units{kCapital, kAlpha, kCapital}) == Units{kSmall, kSmallAlpha, kFinal});
    // A cased letter AFTER it, so not final.
    CHECK(lower(Units{kAlpha, kCapital, kAlpha}) == Units{kSmallAlpha, kSmall, kSmallAlpha});
    // U+002E FULL STOP is Case_Ignorable, so it does not end the run and does
    // not make the sigma non-final either.
    CHECK(lower(Units{kAlpha, kCapital, '.'}) == Units{kSmallAlpha, kFinal, '.'});
    // Both lowercase sigmas uppercase back to the one capital.
    CHECK(upper(Units{kFinal}) == Units{kCapital});
    CHECK(upper(Units{kSmall}) == Units{kCapital});
}

TEST_CASE("case conversion crosses the surrogate pair and the whole BMP") {
    // U+10400 DESERET CAPITAL LONG I <-> U+10428, as surrogate pairs.
    const Units capitalDeseret = {0xD801, 0xDC00};
    const Units smallDeseret = {0xD801, 0xDC28};
    CHECK(lower(capitalDeseret) == smallDeseret);
    CHECK(upper(smallDeseret) == capitalDeseret);
    // Cherokee: the uppercase letters sit BELOW the lowercase ones.
    CHECK(lower(Units{0x13A0}) == Units{0xAB70});
    CHECK(upper(Units{0xAB70}) == Units{0x13A0});
    // U+00B5 MICRO SIGN uppercases out of Latin-1 into Greek.
    CHECK(upper(Units{0x00B5}) == Units{0x039C});
}

TEST_CASE("an unpaired surrogate survives a case conversion unchanged") {
    // It is not a code point pair, so 11.1.4 reads it as itself and it
    // re-encodes to the same unit -- a case conversion is not a repair.
    CHECK(upper(Units{0xD800}) == Units{0xD800});
    CHECK(lower(Units{0xDFFF}) == Units{0xDFFF});
    CHECK(upper(Units{0xD800, 'a'}) == Units{0xD800, 'A'});
    CHECK(lower(Units{'A', 0xDC00}) == Units{'a', 0xDC00});
}
