// Unicode normalization, tested at the seam below String.prototype.normalize:
// units in, units out, no heap and no JS. The member adds the form argument's
// coercion, the RangeError and the receiver-returning fast path, which the
// oracle case tests/oracle/cases/string_normalize.js pins; what is about the
// TABLES and the algorithm is asked here, where a failure names the mapping.
//
// Expectations are UAX #15's and NormalizationTest.txt's (UCD 16.0.0).

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "runtime/unicode_norm.h"

using namespace bronze::runtime;
using unicode::NormForm;

namespace {

using Units = std::vector<uint16_t>;

Units nfc(const Units& in) { return unicode::normalize(in, NormForm::NFC); }
Units nfd(const Units& in) { return unicode::normalize(in, NormForm::NFD); }
Units nfkc(const Units& in) { return unicode::normalize(in, NormForm::NFKC); }
Units nfkd(const Units& in) { return unicode::normalize(in, NormForm::NFKD); }

}  // namespace

TEST_CASE("ASCII is every form of itself") {
    const Units ascii = {'a', 'B', ' ', '1', '~'};
    CHECK(nfc(ascii) == ascii);
    CHECK(nfd(ascii) == ascii);
    CHECK(nfkc(ascii) == ascii);
    CHECK(nfkd(ascii) == ascii);
    CHECK(nfc(Units{}) == Units{});
    CHECK(unicode::isTriviallyNormalized(ascii.data(), ascii.size(), NormForm::NFKD));
}

TEST_CASE("canonical decomposition and recomposition of Latin letters") {
    CHECK(nfd(Units{0x00E9}) == Units{'e', 0x0301});
    CHECK(nfd(Units{0x00C5}) == Units{'A', 0x030A});
    CHECK(nfd(Units{0x00F1}) == Units{'n', 0x0303});
    CHECK(nfc(Units{'e', 0x0301}) == Units{0x00E9});
    CHECK(nfc(Units{'A', 0x030A}) == Units{0x00C5});
    // U+01D5 decomposes in two levels: U+00DC U+0304, then U U+0308 U+0304.
    CHECK(nfd(Units{0x01D5}) == Units{'U', 0x0308, 0x0304});
    CHECK(nfc(Units{'U', 0x0308, 0x0304}) == Units{0x01D5});
}

TEST_CASE("canonical ordering sorts non-starters by combining class, stably") {
    // U+0323 (class 220) sorts before U+0307 (class 230) whichever order they
    // arrive in, and NFC then composes the dot below first: U+1EA1, then the
    // dot above is blocked by nothing and stays a combining mark.
    const Units below = {'a', 0x0323, 0x0307};
    const Units above = {'a', 0x0307, 0x0323};
    CHECK(nfd(above) == below);
    CHECK(nfd(below) == below);
    CHECK(nfc(above) == Units{0x1EA1, 0x0307});
    // U+1E0B (d with dot above) + U+0323: NFD reorders to d, 0323, 0307 and
    // NFC recomposes to U+1E0D (dot below) + U+0307.
    CHECK(nfd(Units{0x1E0B, 0x0323}) == Units{'d', 0x0323, 0x0307});
    CHECK(nfc(Units{0x1E0B, 0x0323}) == Units{0x1E0D, 0x0307});
    // Two marks of the same class keep their order: U+0301 and U+0300 are both
    // 230, so neither moves, and only the first composes.
    CHECK(nfd(Units{'a', 0x0301, 0x0300}) == Units{'a', 0x0301, 0x0300});
    CHECK(nfc(Units{'a', 0x0301, 0x0300}) == Units{0x00E1, 0x0300});
}

TEST_CASE("Hangul syllables are arithmetic both ways") {
    CHECK(nfd(Units{0xAC00}) == Units{0x1100, 0x1161});
    CHECK(nfc(Units{0x1100, 0x1161}) == Units{0xAC00});
    // An LVT syllable: U+D7A3 = U+1112 U+1175 U+11C2.
    CHECK(nfd(Units{0xD7A3}) == Units{0x1112, 0x1175, 0x11C2});
    CHECK(nfc(Units{0x1112, 0x1175, 0x11C2}) == Units{0xD7A3});
    // LV + T composes too.
    CHECK(nfc(Units{0xAC00, 0x11A8}) == Units{0xAC01});
}

TEST_CASE("compatibility decomposition") {
    CHECK(nfkd(Units{0xFB01}) == Units{'f', 'i'});
    CHECK(nfkc(Units{0xFB01}) == Units{'f', 'i'});
    CHECK(nfc(Units{0xFB01}) == Units{0xFB01});
    CHECK(nfkc(Units{0x2460}) == Units{'1'});
    // Halfwidth katakana KA becomes the full-width one.
    CHECK(nfkc(Units{0xFF76}) == Units{0x30AB});
    // Halfwidth KA + halfwidth voiced mark composes under NFKC to GA.
    CHECK(nfkc(Units{0xFF76, 0xFF9E}) == Units{0x30AC});
    // A run in the astral planes: MATHEMATICAL BOLD CAPITAL A..Z.
    CHECK(nfkc(Units{0xD835, 0xDC00}) == Units{'A'});
    CHECK(nfkc(Units{0xD835, 0xDC19}) == Units{'Z'});
    // The longest mapping there is, U+FDFA, eighteen code points.
    CHECK(nfkd(Units{0xFDFA}).size() == 18);
    // A compatibility mapping whose result decomposes canonically:
    // U+1E9B (long s with dot above) -> U+017F U+0307 canonically, and U+017F
    // is compatibility s, so NFKD is s + U+0307 and NFKC recomposes U+1E61.
    CHECK(nfkd(Units{0x1E9B}) == Units{'s', 0x0307});
    CHECK(nfkc(Units{0x1E9B}) == Units{0x1E61});
    CHECK(nfc(Units{0x1E9B}) == Units{0x1E9B});
}

TEST_CASE("composition exclusions and singletons") {
    // U+0958 DEVANAGARI QA is on the explicit list: NFC leaves it decomposed.
    CHECK(nfc(Units{0x0958}) == Units{0x0915, 0x093C});
    CHECK(nfc(Units{0x0915, 0x093C}) == Units{0x0915, 0x093C});
    // U+212B ANGSTROM SIGN is a singleton: it maps to U+00C5 and never back.
    CHECK(nfc(Units{0x212B}) == Units{0x00C5});
    CHECK(nfd(Units{0x212B}) == Units{'A', 0x030A});
    // U+2126 OHM -> U+03A9; U+0344 is a non-starter decomposition.
    CHECK(nfc(Units{0x2126}) == Units{0x03A9});
    CHECK(nfc(Units{0x0344}) == Units{0x0308, 0x0301});
    // A post-composition-version exclusion in the astral planes: U+1D15E
    // MUSICAL SYMBOL HALF NOTE = U+1D157 U+1D165, and stays decomposed.
    const Units halfNote = {0xD834, 0xDD5E};
    const Units halfNoteD = {0xD834, 0xDD57, 0xD834, 0xDD65};
    CHECK(nfd(halfNote) == halfNoteD);
    CHECK(nfc(halfNote) == halfNoteD);
}

TEST_CASE("an unpaired surrogate is a starter that passes through") {
    CHECK(nfc(Units{0xD800}) == Units{0xD800});
    CHECK(nfd(Units{'a', 0xDC00, 0x0301}) == Units{'a', 0xDC00, 0x0301});
    // It blocks: the acute after it does not reach back to the `e`.
    CHECK(nfc(Units{'e', 0xD800, 0x0301}) == Units{'e', 0xD800, 0x0301});
    CHECK(nfc(Units{0xD800, 'e', 0x0301}) == Units{0xD800, 0x00E9});
}

TEST_CASE("combining classes and quick-check bounds") {
    CHECK(unicode::combiningClass('a') == 0);
    CHECK(unicode::combiningClass(0x0301) == 230);
    CHECK(unicode::combiningClass(0x0323) == 220);
    CHECK(unicode::combiningClass(0x1D165) == 216);
    CHECK(unicode::combiningClass(0x05B0) == 10);
    const char latin1[] = {'c', static_cast<char>(0xE9)};
    CHECK(unicode::isTriviallyNormalizedLatin1(latin1, 2, NormForm::NFC));
    CHECK_FALSE(unicode::isTriviallyNormalizedLatin1(latin1, 2, NormForm::NFD));
    const Units nbsp = {0x00A0};
    CHECK(unicode::isTriviallyNormalized(nbsp.data(), 1, NormForm::NFC));
    CHECK_FALSE(unicode::isTriviallyNormalized(nbsp.data(), 1, NormForm::NFKC));
    CHECK(nfkc(nbsp) == Units{' '});
}
