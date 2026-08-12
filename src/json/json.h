#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// JSON text, and nothing else.
//
// This is its own module rather than a corner of `src/parse` because JSON is
// not JavaScript (ECMA-262 25.5.1 defers to JSON.parse's own grammar, which
// is RFC 8259's): a trailing comma is legal in a JS object literal and a
// syntax error here; `'x'`, `{a:1}`, `0x10`, `+1`, `.5`, `1.` and `//` are all
// JavaScript and none of them is JSON. Sharing a parser between two grammars
// that differ in what they REJECT is how the rejections get lost, so the two
// share nothing.
//
// It also knows nothing about bronze's value model — no Value, no heap, no
// GC — which is what lets `tests/json` drive it directly and what keeps the
// grammar's decisions provable without a runtime. The caller turns the tree
// below into whatever it wants; `src/runtime/builtin_json.cpp` turns it into
// JavaScript values.

namespace bronze::json {

// The input is a sequence of UTF-16 CODE UNITS, because that is what a
// JavaScript string is and because `𝄞` is defined per code unit: a lone
// surrogate is a legal JSON string element and re-encoding through UTF-8 on the
// way in would either lose it or invent a replacement.
using Units = std::u16string;
using UnitsView = std::u16string_view;

struct Value;
using ValuePtr = std::unique_ptr<Value>;

struct Member {
    Units key;
    ValuePtr value;
};

struct Value {
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    Units text;
    std::vector<ValuePtr> elements;
    // Insertion order, duplicates included: 25.5.1's parse walks the members
    // in order and lets a later one overwrite an earlier one, which is not the
    // same as dropping the earlier one — the property keeps the POSITION its
    // first definition gave it.
    std::vector<Member> members;
};

// Parses one complete JSON text. Returns null and fills `error` on a syntax
// error; the message names what was expected and where, in the house style.
// Trailing content after the value is an error, not a stopping point — the
// project rule is that every parser consumes all its input or says so.
ValuePtr parse(UnitsView text, std::string& error);

}  // namespace bronze::json
