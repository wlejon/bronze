// GENERATED FILE -- DO NOT EDIT BY HAND.
//
// Written by tools/gen_unicode_tables from the Unicode Character Database
// version 16.0.0 vendored under tools/ucd/. To change anything here, change the
// generator and rerun it:
//
//     tools/gen_unicode_tables
//
// The generator asserts the UCD version it reads, so a rerun either reproduces
// these bytes or stops.

#pragma once

#include <cstdint>

namespace bronze::regex::data {

// A closed code point interval.
struct CodeRange {
    uint32_t first;
    uint32_t last;
};

// The binary properties of ECMA-262 Table 69 that carry data (all but `Any`,
// `ASCII` and `Assigned`), each as merged ascending ranges, 11695 in all.
extern const CodeRange kBinASCIIHexDigit[];
extern const CodeRange kBinAlphabetic[];
extern const CodeRange kBinBidiControl[];
extern const CodeRange kBinBidiMirrored[];
extern const CodeRange kBinCaseIgnorable[];
extern const CodeRange kBinCased[];
extern const CodeRange kBinChangesWhenCasefolded[];
extern const CodeRange kBinChangesWhenCasemapped[];
extern const CodeRange kBinChangesWhenLowercased[];
extern const CodeRange kBinChangesWhenNFKCCasefolded[];
extern const CodeRange kBinChangesWhenTitlecased[];
extern const CodeRange kBinChangesWhenUppercased[];
extern const CodeRange kBinDash[];
extern const CodeRange kBinDefaultIgnorableCodePoint[];
extern const CodeRange kBinDeprecated[];
extern const CodeRange kBinDiacritic[];
extern const CodeRange kBinEmoji[];
extern const CodeRange kBinEmojiComponent[];
extern const CodeRange kBinEmojiModifier[];
extern const CodeRange kBinEmojiModifierBase[];
extern const CodeRange kBinEmojiPresentation[];
extern const CodeRange kBinExtendedPictographic[];
extern const CodeRange kBinExtender[];
extern const CodeRange kBinGraphemeBase[];
extern const CodeRange kBinGraphemeExtend[];
extern const CodeRange kBinHexDigit[];
extern const CodeRange kBinIDSBinaryOperator[];
extern const CodeRange kBinIDSTrinaryOperator[];
extern const CodeRange kBinIDContinue[];
extern const CodeRange kBinIDStart[];
extern const CodeRange kBinIdeographic[];
extern const CodeRange kBinJoinControl[];
extern const CodeRange kBinLogicalOrderException[];
extern const CodeRange kBinLowercase[];
extern const CodeRange kBinMath[];
extern const CodeRange kBinNoncharacterCodePoint[];
extern const CodeRange kBinPatternSyntax[];
extern const CodeRange kBinPatternWhiteSpace[];
extern const CodeRange kBinQuotationMark[];
extern const CodeRange kBinRadical[];
extern const CodeRange kBinRegionalIndicator[];
extern const CodeRange kBinSentenceTerminal[];
extern const CodeRange kBinSoftDotted[];
extern const CodeRange kBinTerminalPunctuation[];
extern const CodeRange kBinUnifiedIdeograph[];
extern const CodeRange kBinUppercase[];
extern const CodeRange kBinVariationSelector[];
extern const CodeRange kBinWhiteSpace[];
extern const CodeRange kBinXIDContinue[];
extern const CodeRange kBinXIDStart[];

struct BinaryProperty {
    const char* name;  // canonical
    const CodeRange* ranges;
    uint32_t count;
};

extern const BinaryProperty kBinaryProperties[];
constexpr uint32_t kBinaryPropertyCount = 50;

// Every spelling Table 69 gives (canonical name and alias), sorted, with the
// index into kBinaryProperties it names. 22.2.1 matches exactly.
struct BinaryPropertyName {
    const char* name;
    uint8_t property;
};

extern const BinaryPropertyName kBinaryPropertyNames[];
constexpr uint32_t kBinaryPropertyNameCount = 95;

}  // namespace bronze::regex::data
