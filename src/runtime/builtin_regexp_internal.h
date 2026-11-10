#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "regex/regex.h"
#include "runtime/fn.h"
#include "runtime/rt_builtins.h"
#include "runtime/string.h"
#include "runtime/value.h"

// What the RegExp translation units share and nothing else may see:
// builtin_regexp.cpp (the constructor, `RegExp.prototype` and the flag
// accessors), builtin_regexp_exec.cpp (the compiled-pattern table, `exec`,
// the match array and the `lastIndex` protocol), builtin_regexp_escape.cpp
// (`RegExp.escape`) and builtin_regexp_symbols.cpp (the five symbol-keyed
// members, which reach the matcher through the public `rtRegExpPattern`).

namespace bronze::runtime {

// 22.2.3.3 RegExpInitialize on a header `rtAllocateNativeBaseInstance` made:
// the source and the flags, compiled, with `lastIndex` at zero. Throws the
// SyntaxError when the pattern does not compile, and answers true otherwise.
// `sourceStr` is REWRITTEN in place when 22.2.6.13.1 has to escape it.
bool rtInitializeRegExp(Rooted<Value>& re, Rooted<Value>& sourceStr, const std::string& flagsText);

// 22.2.3.1 + 22.2.3.3 with the intrinsic prototype: what a literal and a
// plain `RegExp(...)` call build. Throws the SyntaxError when the pattern
// does not compile.
Value rtMakeRegExp(Rooted<Value>& sourceStr, const std::string& flagsText);

// 22.2.6.16 `RegExp.prototype.test` without constructing a match array.
bool rtRegExpExecTest(Rooted<Value>& re, Rooted<Value>& inputStr);

// The native bodies builtin_regexp.cpp installs and the other files own.
uint64_t rtRegExpExecBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtRegExpEscapeBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
// 22.2.6.8, .9, .11, .12, .14 as own data properties of `RegExp.prototype`,
// in the clause's order.
void rtInstallRegExpSymbolMethods(Rooted<Value>& proto);

// Cache inspection for testing.
size_t rtRegExpCacheSize();

// UTF-16 view over `s` valid ONLY while no GC allocation runs: zero-copy for
// UTF-16 strings, stack-widened for Latin-1 strings up to 512 units.
class ScopedRegexUnits {
public:
    static constexpr size_t kStackUnits = 512;

    explicit ScopedRegexUnits(const StringHeader* s) {
        const uint32_t len = s->getLength();
        if (s->isUTF16()) {
            view_ = regex::UnitsView(reinterpret_cast<const char16_t*>(s->utf16Data()), len);
            return;
        }
        const auto* src = reinterpret_cast<const uint8_t*>(s->latin1Data());
        char16_t* dst = stackBuf_;
        if (len > kStackUnits) {
            heapBuf_.resize(len);
            dst = heapBuf_.data();
        }
        for (uint32_t i = 0; i < len; ++i) dst[i] = src[i];
        view_ = regex::UnitsView(dst, len);
    }

    ScopedRegexUnits(const ScopedRegexUnits&) = delete;
    ScopedRegexUnits& operator=(const ScopedRegexUnits&) = delete;

    regex::UnitsView view() const noexcept { return view_; }
    size_t size() const noexcept { return view_.size(); }

private:
    char16_t stackBuf_[kStackUnits];
    std::vector<char16_t> heapBuf_;
    regex::UnitsView view_;
};

// UTF-16 view over `s` that remains valid across moving GC allocations: zero-copy
// for immortal arena UTF-16 strings, stack-copied/widened up to 512 units.
class ScopedStableRegexUnits {
public:
    static constexpr size_t kStackUnits = 512;

    explicit ScopedStableRegexUnits(const StringHeader* s) {
        const uint32_t len = s->getLength();
        if (s->isUTF16() && s->isArena()) {
            view_ = regex::UnitsView(reinterpret_cast<const char16_t*>(s->utf16Data()), len);
            return;
        }
        char16_t* dst = stackBuf_;
        if (len > kStackUnits) {
            heapBuf_.resize(len);
            dst = heapBuf_.data();
        }
        if (s->isUTF16()) {
            if (len > 0) std::memcpy(dst, s->utf16Data(), static_cast<size_t>(len) * sizeof(char16_t));
        } else {
            const auto* src = reinterpret_cast<const uint8_t*>(s->latin1Data());
            for (uint32_t i = 0; i < len; ++i) dst[i] = src[i];
        }
        view_ = regex::UnitsView(dst, len);
    }

    ScopedStableRegexUnits(const ScopedStableRegexUnits&) = delete;
    ScopedStableRegexUnits& operator=(const ScopedStableRegexUnits&) = delete;

    regex::UnitsView view() const noexcept { return view_; }
    size_t size() const noexcept { return view_.size(); }
    bool empty() const noexcept { return view_.empty(); }

private:
    char16_t stackBuf_[kStackUnits];
    std::vector<char16_t> heapBuf_;
    regex::UnitsView view_;
};

// Reuses per-thread `MatchResult::captures` capacity across regex calls while
// staying reentrancy-safe if a callback invokes nested regex matching.
class ScratchMatchResult {
public:
    ScratchMatchResult() noexcept { std::swap(result.captures, scratch()); }
    ~ScratchMatchResult() noexcept {
        auto& s = scratch();
        if (result.captures.capacity() > s.capacity()) {
            std::swap(result.captures, s);
        }
    }

    ScratchMatchResult(const ScratchMatchResult&) = delete;
    ScratchMatchResult& operator=(const ScratchMatchResult&) = delete;

    regex::MatchResult result;

private:
    static std::vector<int64_t>& scratch() noexcept {
        static thread_local std::vector<int64_t> buf;
        return buf;
    }
};

}  // namespace bronze::runtime
