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

#include "regex/unicode_data_binary.h"

namespace bronze::regex::data {

extern const CodeRange kStrBasicEmojiRanges[];
extern const uint32_t kStrBasicEmojiSeqs[];
extern const uint32_t kStrEmojiKeycapSequenceSeqs[];
extern const uint32_t kStrRGIEmojiFlagSequenceSeqs[];
extern const uint32_t kStrRGIEmojiModifierSequenceSeqs[];
extern const uint32_t kStrRGIEmojiTagSequenceSeqs[];
extern const uint32_t kStrRGIEmojiZWJSequenceSeqs[];

struct StringPropertyTable {
    const char* name;
    const CodeRange* ranges;
    uint32_t rangeCount;
    const uint32_t* seqData;
    uint32_t seqDataLen;
};

extern const StringPropertyTable kStringProperties[];
constexpr uint32_t kStringPropertyCount = 6;

}  // namespace bronze::regex::data
