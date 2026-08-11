// console.log. Primitives print their own text; a container's format is
// docs/0013's decision and lives in inspect.cpp, which is a recursive walk
// with its own rules and none of them this file's business.

#include <cmath>
#include <cstdio>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/fatal.h"
#include "runtime/number_format.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

void writeUtf8(uint32_t cp) {
    if (cp <= 0x7F) {
        std::fputc(static_cast<char>(cp), stdout);
    } else if (cp <= 0x7FF) {
        std::fputc(static_cast<char>(0xC0 | (cp >> 6)), stdout);
        std::fputc(static_cast<char>(0x80 | (cp & 0x3F)), stdout);
    } else if (cp <= 0xFFFF) {
        std::fputc(static_cast<char>(0xE0 | (cp >> 12)), stdout);
        std::fputc(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), stdout);
        std::fputc(static_cast<char>(0x80 | (cp & 0x3F)), stdout);
    } else {
        std::fputc(static_cast<char>(0xF0 | (cp >> 18)), stdout);
        std::fputc(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)), stdout);
        std::fputc(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), stdout);
        std::fputc(static_cast<char>(0x80 | (cp & 0x3F)), stdout);
    }
}

// A raw string, at the top level, where node prints it unquoted. Latin-1 code
// units above 0x7F are code points in their own right, so both representations
// re-encode rather than copy bytes out.
void writeString(const StringHeader* str) {
    const uint32_t len = str->getLength();
    if (str->isLatin1()) {
        const char* data = str->latin1Data();
        for (uint32_t i = 0; i < len; ++i) {
            writeUtf8(static_cast<unsigned char>(data[i]));
        }
        return;
    }
    const uint16_t* u16 = str->utf16Data();
    for (uint32_t i = 0; i < len; ++i) {
        uint32_t cp = u16[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len) {
            uint32_t low = u16[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        writeUtf8(cp);
    }
}

void writeNumberLine(double num) {
    char buf[64];
    size_t len = 0;
    // console.log distinguishes -0 (inspect formatting), unlike
    // ToString(Number), which yields "0".
    if (num == 0.0 && std::signbit(num)) buf[len++] = '-';
    len += formatJsNumber(num, buf + len);
    buf[len++] = '\n';
    std::fwrite(buf, 1, len, stdout);
}

}  // namespace

extern "C" {

void bronze_print_value(uint64_t valBits) {
    Value v(valBits);
    if (v.isNumber()) {
        writeNumberLine(v.asNumber());
    } else if (v.isInt32()) {
        // Lowering never boxes an Int32 today (docs/0004 decision 1), but an
        // int32 IS a JS number, so it prints as one rather than reaching the
        // object branch if the fast path ever lands.
        writeNumberLine(static_cast<double>(static_cast<int32_t>(v.payload())));
    } else if (v.isString()) {
        writeString(v.asString<StringHeader>());
        std::fputc('\n', stdout);
    } else if (v.isBool()) {
        std::fputs(v.asBool() ? "true\n" : "false\n", stdout);
    } else if (v.isUndefined()) {
        std::fputs("undefined\n", stdout);
    } else if (v.isNull()) {
        std::fputs("null\n", stdout);
    } else if (v.isObject()) {
        const std::string text = rtInspect(v);
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fputc('\n', stdout);
    } else if (v.isHole()) {
        // docs/0004: the hole is internal and never user-visible. Printing it
        // as anything would hide the bug that let it escape.
        fatal("internal: the hole sentinel reached console.log");
    } else if (v.isSymbol()) {
        fatal("printing a symbol is unsupported (bronze has no symbols)");
    } else {
        fatal("internal: console.log reached a value with an unknown tag");
    }
    std::fflush(stdout);
}

void bronze_print_string(const char* s) {
    if (s) std::fputs(s, stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

}  // extern "C"

}  // namespace bronze::runtime
