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

#include "regex/unicode_data_binary.h"

namespace bronze::regex::data {

const BinaryProperty kBinaryProperties[] = {
    {"ASCII_Hex_Digit", kBinASCIIHexDigit, 3},
    {"Alphabetic", kBinAlphabetic, 757},
    {"Bidi_Control", kBinBidiControl, 4},
    {"Bidi_Mirrored", kBinBidiMirrored, 114},
    {"Case_Ignorable", kBinCaseIgnorable, 452},
    {"Cased", kBinCased, 159},
    {"Changes_When_Casefolded", kBinChangesWhenCasefolded, 626},
    {"Changes_When_Casemapped", kBinChangesWhenCasemapped, 131},
    {"Changes_When_Lowercased", kBinChangesWhenLowercased, 614},
    {"Changes_When_NFKC_Casefolded", kBinChangesWhenNFKCCasefolded, 844},
    {"Changes_When_Titlecased", kBinChangesWhenTitlecased, 629},
    {"Changes_When_Uppercased", kBinChangesWhenUppercased, 630},
    {"Dash", kBinDash, 24},
    {"Default_Ignorable_Code_Point", kBinDefaultIgnorableCodePoint, 17},
    {"Deprecated", kBinDeprecated, 8},
    {"Diacritic", kBinDiacritic, 214},
    {"Emoji", kBinEmoji, 150},
    {"Emoji_Component", kBinEmojiComponent, 10},
    {"Emoji_Modifier", kBinEmojiModifier, 1},
    {"Emoji_Modifier_Base", kBinEmojiModifierBase, 40},
    {"Emoji_Presentation", kBinEmojiPresentation, 80},
    {"Extended_Pictographic", kBinExtendedPictographic, 78},
    {"Extender", kBinExtender, 41},
    {"Grapheme_Base", kBinGraphemeBase, 894},
    {"Grapheme_Extend", kBinGraphemeExtend, 375},
    {"Hex_Digit", kBinHexDigit, 6},
    {"IDS_Binary_Operator", kBinIDSBinaryOperator, 3},
    {"IDS_Trinary_Operator", kBinIDSTrinaryOperator, 1},
    {"ID_Continue", kBinIDContinue, 793},
    {"ID_Start", kBinIDStart, 677},
    {"Ideographic", kBinIdeographic, 21},
    {"Join_Control", kBinJoinControl, 1},
    {"Logical_Order_Exception", kBinLogicalOrderException, 7},
    {"Lowercase", kBinLowercase, 675},
    {"Math", kBinMath, 139},
    {"Noncharacter_Code_Point", kBinNoncharacterCodePoint, 18},
    {"Pattern_Syntax", kBinPatternSyntax, 28},
    {"Pattern_White_Space", kBinPatternWhiteSpace, 5},
    {"Quotation_Mark", kBinQuotationMark, 13},
    {"Radical", kBinRadical, 3},
    {"Regional_Indicator", kBinRegionalIndicator, 1},
    {"Sentence_Terminal", kBinSentenceTerminal, 88},
    {"Soft_Dotted", kBinSoftDotted, 34},
    {"Terminal_Punctuation", kBinTerminalPunctuation, 116},
    {"Unified_Ideograph", kBinUnifiedIdeograph, 17},
    {"Uppercase", kBinUppercase, 656},
    {"Variation_Selector", kBinVariationSelector, 4},
    {"White_Space", kBinWhiteSpace, 10},
    {"XID_Continue", kBinXIDContinue, 800},
    {"XID_Start", kBinXIDStart, 684},
};

const BinaryPropertyName kBinaryPropertyNames[] = {
    {"AHex", 0}, {"ASCII_Hex_Digit", 0}, {"Alpha", 1},
    {"Alphabetic", 1}, {"Bidi_C", 2}, {"Bidi_Control", 2},
    {"Bidi_M", 3}, {"Bidi_Mirrored", 3}, {"CI", 4},
    {"CWCF", 6}, {"CWCM", 7}, {"CWKCF", 9},
    {"CWL", 8}, {"CWT", 10}, {"CWU", 11},
    {"Case_Ignorable", 4}, {"Cased", 5}, {"Changes_When_Casefolded", 6},
    {"Changes_When_Casemapped", 7}, {"Changes_When_Lowercased", 8}, {"Changes_When_NFKC_Casefolded", 9},
    {"Changes_When_Titlecased", 10}, {"Changes_When_Uppercased", 11}, {"DI", 13},
    {"Dash", 12}, {"Default_Ignorable_Code_Point", 13}, {"Dep", 14},
    {"Deprecated", 14}, {"Dia", 15}, {"Diacritic", 15},
    {"EBase", 19}, {"EComp", 17}, {"EMod", 18},
    {"EPres", 20}, {"Emoji", 16}, {"Emoji_Component", 17},
    {"Emoji_Modifier", 18}, {"Emoji_Modifier_Base", 19}, {"Emoji_Presentation", 20},
    {"Ext", 22}, {"ExtPict", 21}, {"Extended_Pictographic", 21},
    {"Extender", 22}, {"Gr_Base", 23}, {"Gr_Ext", 24},
    {"Grapheme_Base", 23}, {"Grapheme_Extend", 24}, {"Hex", 25},
    {"Hex_Digit", 25}, {"IDC", 28}, {"IDS", 29},
    {"IDSB", 26}, {"IDST", 27}, {"IDS_Binary_Operator", 26},
    {"IDS_Trinary_Operator", 27}, {"ID_Continue", 28}, {"ID_Start", 29},
    {"Ideo", 30}, {"Ideographic", 30}, {"Join_C", 31},
    {"Join_Control", 31}, {"LOE", 32}, {"Logical_Order_Exception", 32},
    {"Lower", 33}, {"Lowercase", 33}, {"Math", 34},
    {"NChar", 35}, {"Noncharacter_Code_Point", 35}, {"Pat_Syn", 36},
    {"Pat_WS", 37}, {"Pattern_Syntax", 36}, {"Pattern_White_Space", 37},
    {"QMark", 38}, {"Quotation_Mark", 38}, {"RI", 40},
    {"Radical", 39}, {"Regional_Indicator", 40}, {"SD", 42},
    {"STerm", 41}, {"Sentence_Terminal", 41}, {"Soft_Dotted", 42},
    {"Term", 43}, {"Terminal_Punctuation", 43}, {"UIdeo", 44},
    {"Unified_Ideograph", 44}, {"Upper", 45}, {"Uppercase", 45},
    {"VS", 46}, {"Variation_Selector", 46}, {"White_Space", 47},
    {"XIDC", 48}, {"XIDS", 49}, {"XID_Continue", 48},
    {"XID_Start", 49}, {"space", 47},
};

}  // namespace bronze::regex::data
