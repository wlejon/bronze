#pragma once

#include <cstdint>
#include <string_view>

#include "lex/lexer.h"

namespace bronze {

inline DecodedCodePoint decodeUtf8(std::string_view text, size_t pos) {
    if (pos >= text.size()) return {0, 0};
    const auto b0 = static_cast<unsigned char>(text[pos]);
    if (b0 < 0x80) return {b0, 1};
    if ((b0 & 0xE0) == 0xC0 && pos + 1 < text.size()) {
        const auto b1 = static_cast<unsigned char>(text[pos + 1]);
        if ((b1 & 0xC0) == 0x80) {
            uint32_t cp = ((b0 & 0x1F) << 6) | (b1 & 0x3F);
            if (cp >= 0x80) return {cp, 2};
        }
    } else if ((b0 & 0xF0) == 0xE0 && pos + 2 < text.size()) {
        const auto b1 = static_cast<unsigned char>(text[pos + 1]);
        const auto b2 = static_cast<unsigned char>(text[pos + 2]);
        if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80) {
            uint32_t cp = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
            if (cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF)) return {cp, 3};
        }
    } else if ((b0 & 0xF8) == 0xF0 && pos + 3 < text.size()) {
        const auto b1 = static_cast<unsigned char>(text[pos + 1]);
        const auto b2 = static_cast<unsigned char>(text[pos + 2]);
        const auto b3 = static_cast<unsigned char>(text[pos + 3]);
        if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80) {
            uint32_t cp = ((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
            if (cp >= 0x10000 && cp <= 0x10FFFF) return {cp, 4};
        }
    }
    return {0, 0};
}

inline bool isUnicodeWhitespace(uint32_t cp) {
    return cp == 0x00A0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
           cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F ||
           cp == 0x3000 || cp == 0xFEFF;
}

inline bool isUnicodePunctuation(uint32_t cp) {
    if (cp == 0x00AB || cp == 0x00BB) return true;
    if (cp >= 0x2010 && cp <= 0x2027) return true;
    if (cp >= 0x2030 && cp <= 0x205E) return true;
    if (cp >= 0x3001 && cp <= 0x3003) return true;
    if (cp >= 0xFF01 && cp <= 0xFF0F) return true;
    if (cp >= 0xFF1A && cp <= 0xFF20) return true;
    if (cp >= 0xFF3B && cp <= 0xFF40) return true;
    if (cp >= 0xFF5B && cp <= 0xFF65) return true;
    return false;
}

inline bool isIdentStart(uint32_t cp) {
    if (cp < 0x80) {
        return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp == '_' || cp == '$';
    }
    return !isUnicodeWhitespace(cp) && !isUnicodePunctuation(cp);
}

inline bool isIdentPart(uint32_t cp) {
    if (cp < 0x80) {
        return isIdentStart(cp) || (cp >= '0' && cp <= '9');
    }
    return !isUnicodeWhitespace(cp) && !isUnicodePunctuation(cp);
}

}  // namespace bronze
