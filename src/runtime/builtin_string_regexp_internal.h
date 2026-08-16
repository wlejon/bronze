#pragma once

// The pieces shared by the two halves of the string/regexp protocol: the
// String.prototype members that TRIAGE a pattern argument
// (builtin_string_regexp.cpp) and the RegExp.prototype symbol-keyed members
// that IMPLEMENT the algorithms (builtin_regexp_symbols.cpp).
//
// What lives here is everything that answers "given one match, what text comes
// out": 22.1.3.19.1 GetSubstitution and the replacer-call protocol around it.
// Both halves need it because the two replacement algorithms are genuinely
// different — a string search value is matched LITERALLY and never reaches the
// matcher (22.1.3.19 step 3), a RegExp one runs 22.2.6.11 — while `$&`, `$1`
// and a function replacer mean exactly the same thing in both. One copy, so
// they cannot disagree.
//
// UTF-16 code units are the currency here, as everywhere in builtin_string.cpp:
// a string is stored Latin-1 or UTF-16 and every result is rebuilt from units.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/object.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {
namespace string_regexp {

using Units = std::vector<uint16_t>;

inline Units unitsOf(Value str) { return rtStringUnits(str.asString<StringHeader>()); }

inline Value stringOf(const Units& units) { return rtStringFromUnits(units); }

inline Units slice(const Units& units, size_t from, size_t to) {
    if (from > units.size()) from = units.size();
    if (to > units.size()) to = units.size();
    if (to < from) to = from;
    return Units(units.begin() + static_cast<std::ptrdiff_t>(from),
                 units.begin() + static_cast<std::ptrdiff_t>(to));
}

inline bool isCallable(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function;
}

// One match, reduced to what a replacement needs. Holding the pieces rather
// than a match array is what lets `replace` with a `g` pattern run over a long
// string without allocating an array per match.
struct MatchPieces {
    size_t start = 0;
    size_t end = 0;
    std::vector<Units> captures;
    std::vector<bool> present;
    std::vector<std::string> names;
};

// 22.1.3.19.1 GetSubstitution: `$$`, `$&`, `` $` ``, `$'`, `$1`..`$99` and
// `$<name>`. A `$` followed by anything else is a literal `$`, which is the
// specification's rule and not a fallback — `"a".replace("a", "$x")` is "$x".
inline Units substitute(const Units& replacement, const Units& input, size_t matchStart,
                        size_t matchEnd, const std::vector<Units>& captures,
                        const std::vector<bool>& capturePresent,
                        const std::vector<std::string>& captureNames) {
    Units out;
    for (size_t i = 0; i < replacement.size(); ++i) {
        if (replacement[i] != '$' || i + 1 == replacement.size()) {
            out.push_back(replacement[i]);
            continue;
        }
        const uint16_t next = replacement[i + 1];
        if (next == '$') {
            out.push_back('$');
            ++i;
            continue;
        }
        if (next == '&') {
            const Units whole = slice(input, matchStart, matchEnd);
            out.insert(out.end(), whole.begin(), whole.end());
            ++i;
            continue;
        }
        if (next == '`') {
            const Units before = slice(input, 0, matchStart);
            out.insert(out.end(), before.begin(), before.end());
            ++i;
            continue;
        }
        if (next == '\'') {
            const Units after = slice(input, matchEnd, input.size());
            out.insert(out.end(), after.begin(), after.end());
            ++i;
            continue;
        }
        if (next == '<' && !captureNames.empty()) {
            size_t close = i + 2;
            std::string name;
            while (close < replacement.size() && replacement[close] != '>') {
                name.push_back(static_cast<char>(replacement[close]));
                ++close;
            }
            if (close < replacement.size()) {
                i = close;
                for (size_t g = 0; g < captureNames.size(); ++g) {
                    if (captureNames[g] != name) continue;
                    if (capturePresent[g]) {
                        out.insert(out.end(), captures[g].begin(), captures[g].end());
                    }
                    break;
                }
                continue;
            }
            // No `>`: the `$<` is literal text, which is what the grammar
            // says when the production does not complete.
            out.push_back('$');
            continue;
        }
        if (next >= '0' && next <= '9') {
            // Two digits when they name a group that exists, one otherwise:
            // `$12` is group 12 in a pattern with twelve groups and group 1
            // followed by "2" in a pattern with one.
            size_t index = static_cast<size_t>(next - '0');
            size_t consumed = 1;
            if (i + 2 < replacement.size() && replacement[i + 2] >= '0' &&
                replacement[i + 2] <= '9') {
                const size_t twoDigit = index * 10 + static_cast<size_t>(replacement[i + 2] - '0');
                if (twoDigit >= 1 && twoDigit <= captures.size()) {
                    index = twoDigit;
                    consumed = 2;
                }
            }
            if (index >= 1 && index <= captures.size()) {
                if (capturePresent[index - 1]) {
                    out.insert(out.end(), captures[index - 1].begin(), captures[index - 1].end());
                }
                i += consumed;
                continue;
            }
        }
        out.push_back('$');
    }
    return out;
}

// The replacer function's arguments (22.1.3.19 step 14.g): the matched text,
// then every capture, then the offset, then the whole input, then the groups
// object when the pattern has named groups.
inline Value callReplacer(Rooted<Value>& fn, const MatchPieces& pieces, const Units& input) {
    Rooted<Value> matched{stringOf(slice(input, pieces.start, pieces.end))};
    std::vector<Rooted<Value>> roots;
    roots.reserve(pieces.captures.size());
    for (size_t g = 0; g < pieces.captures.size(); ++g) {
        roots.emplace_back(pieces.present[g] ? stringOf(pieces.captures[g])
                                             : Value::fromUndefined());
    }
    Rooted<Value> whole{stringOf(input)};
    Rooted<Value> groups;
    if (!pieces.names.empty()) {
        groups.set(Value(bronze_create_object()));
        for (size_t g = 0; g < pieces.names.size(); ++g) {
            if (pieces.names[g].empty()) continue;
            Rooted<Value> key{rtMakeString(pieces.names[g])};
            Rooted<Value> value{pieces.present[g] ? stringOf(pieces.captures[g])
                                                  : Value::fromUndefined()};
            groups.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, value);
        }
    }

    // The argument vector is filled only once every allocation above is done.
    // A Value read out of its root any earlier is a raw pointer, and the next
    // allocation to trigger a collection moves what it points at — leaving a
    // replacer that is handed the right number of arguments and the wrong
    // bytes.
    std::vector<Value> argv;
    argv.reserve(roots.size() + 4);
    argv.push_back(matched.get());
    for (auto& r : roots) argv.push_back(r.get());
    argv.push_back(Value::fromDouble(static_cast<double>(pieces.start)));
    argv.push_back(whole.get());
    if (!pieces.names.empty()) argv.push_back(groups.get());
    Rooted<Value> undefinedThis;
    return Value(bronze_dynamic_call(fn.get().rawBits(), undefinedThis.get().rawBits(),
                                     static_cast<uint32_t>(argv.size()),
                                     reinterpret_cast<const uint64_t*>(argv.data())));
}

// One replacement, appended to `out`. Shared by every combination of
// (string pattern, RegExp pattern) x (string replacement, function
// replacement), which is why it takes the pieces rather than a match.
inline bool appendReplacement(Units& out, const Units& input, const MatchPieces& pieces,
                              Rooted<Value>& replacement, bool replacerIsFunction) {
    if (!replacerIsFunction) {
        const Units text = unitsOf(replacement.get());
        const Units expanded = substitute(text, input, pieces.start, pieces.end, pieces.captures,
                                          pieces.present, pieces.names);
        out.insert(out.end(), expanded.begin(), expanded.end());
        return true;
    }
    Value produced = callReplacer(replacement, pieces, input);
    if (rtExceptionPending()) return false;
    Rooted<Value> text{rtValueToString(produced)};
    const Units piece = unitsOf(text.get());
    out.insert(out.end(), piece.begin(), piece.end());
    return true;
}

}  // namespace string_regexp
}  // namespace bronze::runtime
