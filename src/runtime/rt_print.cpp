// console.log / console.warn / console.error. Primitives print their own
// text; a container's format is docs/0013's decision and lives in
// inspect.cpp, which is a recursive walk with its own rules and none of them
// this file's business.
//
// The destination is a parameter threaded through every writer rather than a
// module-level `FILE*` the entry points swap: `console.warn` and
// `console.log` must format a value IDENTICALLY, so they share one formatter,
// and a stream held in a variable is a piece of state that a future early
// return could leave pointing at the wrong file.

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

void writeUtf8(uint32_t cp, std::FILE* out) {
    if (cp <= 0x7F) {
        std::fputc(static_cast<char>(cp), out);
    } else if (cp <= 0x7FF) {
        std::fputc(static_cast<char>(0xC0 | (cp >> 6)), out);
        std::fputc(static_cast<char>(0x80 | (cp & 0x3F)), out);
    } else if (cp <= 0xFFFF) {
        std::fputc(static_cast<char>(0xE0 | (cp >> 12)), out);
        std::fputc(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), out);
        std::fputc(static_cast<char>(0x80 | (cp & 0x3F)), out);
    } else {
        std::fputc(static_cast<char>(0xF0 | (cp >> 18)), out);
        std::fputc(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)), out);
        std::fputc(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), out);
        std::fputc(static_cast<char>(0x80 | (cp & 0x3F)), out);
    }
}

// A raw string, at the top level, where node prints it unquoted. Latin-1 code
// units above 0x7F are code points in their own right, so both representations
// re-encode rather than copy bytes out.
void writeString(const StringHeader* str, std::FILE* out) {
    const uint32_t len = str->getLength();
    if (str->isLatin1()) {
        const char* data = str->latin1Data();
        for (uint32_t i = 0; i < len; ++i) {
            writeUtf8(static_cast<unsigned char>(data[i]), out);
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
        writeUtf8(cp, out);
    }
}

void writeNumber(double num, std::FILE* out) {
    char buf[64];
    size_t len = 0;
    // console.log distinguishes -0 (inspect formatting), unlike
    // ToString(Number), which yields "0".
    if (num == 0.0 && std::signbit(num)) buf[len++] = '-';
    len += formatJsNumber(num, buf + len);
    std::fwrite(buf, 1, len, out);
}

// One argument's worth of console.log output, with no line terminator. The
// formatter, and the only one: `console.log(a)` and `console.log(a, b)` must
// format `a` identically, so a second spelling of these rules for the
// multi-argument case would be a drift waiting to happen — the same argument
// that put `**` and `Math.pow` on one `rtExponentiate` (docs/0015 decision 3).
void writeValue(uint64_t valBits, std::FILE* out) {
    Value v(valBits);
    if (v.isNumber()) {
        writeNumber(v.asNumber(), out);
    } else if (v.isInt32()) {
        // Lowering never boxes an Int32 today (docs/0004 decision 1), but an
        // int32 IS a JS number, so it prints as one rather than reaching the
        // object branch if the fast path ever lands.
        writeNumber(static_cast<double>(static_cast<int32_t>(v.payload())), out);
    } else if (v.isString()) {
        writeString(v.asString<StringHeader>(), out);
    } else if (v.isBool()) {
        std::fputs(v.asBool() ? "true" : "false", out);
    } else if (v.isUndefined()) {
        std::fputs("undefined", out);
    } else if (v.isNull()) {
        std::fputs("null", out);
    } else if (v.isObject()) {
        const std::string text = rtInspect(v);
        std::fwrite(text.data(), 1, text.size(), out);
    } else if (v.isHole()) {
        // docs/0004: the hole is internal and never user-visible. Printing it
        // as anything would hide the bug that let it escape.
        fatal("internal: the hole sentinel reached console.log");
    } else if (v.isSymbol()) {
        fatal("printing a symbol is unsupported (bronze has no symbols)");
    } else {
        fatal("internal: console.log reached a value with an unknown tag");
    }
}

// One console call: every argument through `writeValue`, joined with a SINGLE
// space and terminated with one newline. Zero arguments is a bare newline:
// there is nothing to join and the line still ends.
void writeLine(uint32_t argc, const uint64_t* argv, std::FILE* out) {
    for (uint32_t i = 0; i < argc; ++i) {
        if (i > 0) std::fputc(' ', out);
        writeValue(argv[i], out);
    }
    std::fputc('\n', out);
    std::fflush(out);
}

}  // namespace

extern "C" {

void bronze_print_value(uint64_t valBits) {
    writeValue(valBits, stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void bronze_print_values(uint32_t argc, const uint64_t* argv) {
    writeLine(argc, argv, stdout);
}

// `console.warn` and `console.error`, which differ from `console.log` in the
// destination and in nothing else: same formatter, same joining, same
// terminator. stderr is not a detail — the oracle harness pins stdout
// byte-for-byte (docs/0003), so a library's warnings landing there would make
// every case built over that library pin the chatter as expected output.
void bronze_print_value_err(uint64_t valBits) {
    writeValue(valBits, stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

void bronze_print_values_err(uint32_t argc, const uint64_t* argv) {
    writeLine(argc, argv, stderr);
}

void bronze_print_string(const char* s) {
    if (s) std::fputs(s, stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

}  // extern "C"

}  // namespace bronze::runtime
