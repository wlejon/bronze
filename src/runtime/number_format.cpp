#include "runtime/number_format.h"

#include <charconv>
#include <cmath>
#include <cstring>

namespace bronze {

void jsShortestDigits(double x, char* digits, int& count, int& exp10) {
    // Shortest round-trip digits via scientific form: "d[.ddd...]e±EE".
    char sci[40];
    auto res = std::to_chars(sci, sci + sizeof(sci), x, std::chars_format::scientific);
    int k = 0;
    const char* p = sci;
    if (*p == '-') ++p;
    digits[k++] = *p++;
    if (*p == '.') {
        ++p;
        while (*p != 'e') digits[k++] = *p++;
    }
    ++p;  // 'e'
    const bool expNeg = (*p == '-');
    if (*p == '+' || *p == '-') ++p;
    int e = 0;
    while (p < res.ptr) e = e * 10 + (*p++ - '0');
    count = k;
    exp10 = expNeg ? -e : e;
}

size_t formatJsNumber(double x, char* out) {
    if (std::isnan(x)) {
        std::memcpy(out, "NaN", 3);
        return 3;
    }
    if (x == 0.0) {  // covers -0: JS String(-0) is "0"
        out[0] = '0';
        return 1;
    }
    size_t pos = 0;
    if (x < 0.0) {
        out[pos++] = '-';
        x = -x;
    }
    if (std::isinf(x)) {
        std::memcpy(out + pos, "Infinity", 8);
        return pos + 8;
    }

    char digits[20];
    int k = 0;
    int exp10 = 0;
    jsShortestDigits(x, digits, k, exp10);

    // ECMA-262 Number::toString: s = digits (k of them), n = exp10 + 1,
    // value = s * 10^(n-k).
    int n = exp10 + 1;
    if (k <= n && n <= 21) {
        std::memcpy(out + pos, digits, static_cast<size_t>(k));
        pos += static_cast<size_t>(k);
        for (int i = 0; i < n - k; ++i) out[pos++] = '0';
    } else if (0 < n && n <= 21) {
        std::memcpy(out + pos, digits, static_cast<size_t>(n));
        pos += static_cast<size_t>(n);
        out[pos++] = '.';
        std::memcpy(out + pos, digits + n, static_cast<size_t>(k - n));
        pos += static_cast<size_t>(k - n);
    } else if (-6 < n && n <= 0) {
        out[pos++] = '0';
        out[pos++] = '.';
        for (int i = 0; i < -n; ++i) out[pos++] = '0';
        std::memcpy(out + pos, digits, static_cast<size_t>(k));
        pos += static_cast<size_t>(k);
    } else {
        out[pos++] = digits[0];
        if (k > 1) {
            out[pos++] = '.';
            std::memcpy(out + pos, digits + 1, static_cast<size_t>(k - 1));
            pos += static_cast<size_t>(k - 1);
        }
        out[pos++] = 'e';
        int e = n - 1;
        out[pos++] = (e < 0) ? '-' : '+';
        if (e < 0) e = -e;
        char expBuf[8];
        auto expRes = std::to_chars(expBuf, expBuf + sizeof(expBuf), e);
        std::memcpy(out + pos, expBuf, static_cast<size_t>(expRes.ptr - expBuf));
        pos += static_cast<size_t>(expRes.ptr - expBuf);
    }
    return pos;
}

}  // namespace bronze
