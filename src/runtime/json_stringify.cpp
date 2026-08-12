// `JSON.stringify` — ECMA-262 25.5.2.
//
// The format is NOT console.log's, which is an inspect format with deliberate
// divergences from node, because its output is for a human reading a terminal;
// this output is data another program parses, so it has no divergences
// available to it. The two therefore share no code, and the differences are the
// point rather than an oversight: quoted keys, no space unless `space` asked
// for one, `"a"` where inspect writes `'a'`, `null` for NaN and both
// infinities, and `undefined` omitted from an object but written as `null` in
// an array.
//
// The order of an object's members is own-enumerable order, which already fixes
// and `bronze_object_keys` already answers — this file never sorts and never
// re-derives it.

#include <cmath>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/number_format.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

using Units = std::vector<uint16_t>;

bool isCallable(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function;
}

bool isArray(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Array;
}

bool isNumberLike(Value v) { return v.isNumber() || v.isInt32(); }

double numberOf(Value v) {
    return v.isInt32() ? static_cast<double>(static_cast<int32_t>(v.payload())) : v.asNumber();
}

void appendAscii(Units& out, const char* text) {
    for (const char* p = text; *p; ++p) out.push_back(static_cast<uint16_t>(*p));
}

void appendAscii(Units& out, const std::string& text) {
    for (char c : text) out.push_back(static_cast<uint16_t>(static_cast<unsigned char>(c)));
}

// 25.5.2.2 QuoteJSONString, which is defined per CODE UNIT and not per code
// point. A well-formed surrogate PAIR passes through whole, so an emoji comes
// out as itself; an unpaired surrogate is escaped, because the result has to
// be well-formed text that re-parses to the same string. Escaping per code
// point would either drop the unpaired half or replace it with U+FFFD, and
// both of those lose data JSON is obliged to carry.
void quoteJsonString(const Units& units, Units& out) {
    static const char* kHex = "0123456789abcdef";
    out.push_back(u'"');
    for (size_t i = 0; i < units.size(); ++i) {
        const uint16_t c = units[i];
        switch (c) {
            case 0x08: appendAscii(out, "\\b"); continue;
            case 0x09: appendAscii(out, "\\t"); continue;
            case 0x0A: appendAscii(out, "\\n"); continue;
            case 0x0C: appendAscii(out, "\\f"); continue;
            case 0x0D: appendAscii(out, "\\r"); continue;
            case 0x22: appendAscii(out, "\\\""); continue;
            case 0x5C: appendAscii(out, "\\\\"); continue;
            default: break;
        }
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < units.size() && units[i + 1] >= 0xDC00 &&
            units[i + 1] <= 0xDFFF) {
            out.push_back(c);
            out.push_back(units[i + 1]);
            ++i;
            continue;
        }
        if (c < 0x20 || (c >= 0xD800 && c <= 0xDFFF)) {
            appendAscii(out, "\\u");
            out.push_back(static_cast<uint16_t>(kHex[(c >> 12) & 0xF]));
            out.push_back(static_cast<uint16_t>(kHex[(c >> 8) & 0xF]));
            out.push_back(static_cast<uint16_t>(kHex[(c >> 4) & 0xF]));
            out.push_back(static_cast<uint16_t>(kHex[c & 0xF]));
            continue;
        }
        out.push_back(c);
    }
    out.push_back(u'"');
}

struct State {
    Units indent;
    Units gap;
    // A POINTER into the caller's rooted slot rather than a Value, because a
    // replacer survives every call it makes and a raw copy here would name
    // where it used to be after the first collection inside one.
    Value* replacer = nullptr;
    bool hasPropertyList = false;
    std::vector<Units> propertyList;
    // Pointers into the rooted slots the recursion holds, for the same reason.
    std::vector<Value*> stack;
};

bool serializeProperty(State& state, const Units& key, Rooted<Value>& holder, Units& out);

bool alreadyOnStack(const State& state, Value v) {
    for (const Value* slot : state.stack) {
        if (slot->rawBits() == v.rawBits()) return true;
    }
    return false;
}

// The two container writers differ only in their brackets and in what an
// `undefined` member does, so the bracket-and-indent half is written once.
void joinPartial(const std::vector<Units>& partial, const Units& indent, const Units& stepback,
                 const Units& gap, char16_t open, char16_t close, Units& out) {
    if (partial.empty()) {
        out.push_back(static_cast<uint16_t>(open));
        out.push_back(static_cast<uint16_t>(close));
        return;
    }
    out.push_back(static_cast<uint16_t>(open));
    if (gap.empty()) {
        for (size_t i = 0; i < partial.size(); ++i) {
            if (i != 0) out.push_back(u',');
            out.insert(out.end(), partial[i].begin(), partial[i].end());
        }
        out.push_back(static_cast<uint16_t>(close));
        return;
    }
    out.push_back(u'\n');
    for (size_t i = 0; i < partial.size(); ++i) {
        if (i != 0) {
            out.push_back(u',');
            out.push_back(u'\n');
        }
        out.insert(out.end(), indent.begin(), indent.end());
        out.insert(out.end(), partial[i].begin(), partial[i].end());
    }
    out.push_back(u'\n');
    out.insert(out.end(), stepback.begin(), stepback.end());
    out.push_back(static_cast<uint16_t>(close));
}

// 25.5.2.5 SerializeJSONArray. `undefined`, a function and a hole all become
// `null` here — an array's shape is its length, so there is nothing to omit.
bool serializeArray(State& state, Rooted<Value>& value, Units& out) {
    if (alreadyOnStack(state, value.get())) {
        rtThrowTypeError("Converting circular structure to JSON");
        return false;
    }
    state.stack.push_back(value.slot_ptr());
    const Units stepback = state.indent;
    state.indent.insert(state.indent.end(), state.gap.begin(), state.gap.end());

    std::vector<Units> partial;
    bool failed = false;
    const uint32_t length = value.get().asObject<ArrayHeader>()->length;
    for (uint32_t i = 0; i < length; ++i) {
        Units key;
        appendAscii(key, std::to_string(i));
        Units element;
        if (!serializeProperty(state, key, value, element)) {
            if (rtExceptionPending()) {
                failed = true;
                break;
            }
            appendAscii(element, "null");
        }
        partial.push_back(std::move(element));
    }
    if (!failed) joinPartial(partial, state.indent, stepback, state.gap, u'[', u']', out);

    state.stack.pop_back();
    state.indent = stepback;
    return !failed;
}

// 25.5.2.4 SerializeJSONObject. A member whose serialization is `undefined` is
// OMITTED, which is the one place an object and an array differ.
bool serializeObject(State& state, Rooted<Value>& value, Units& out) {
    if (alreadyOnStack(state, value.get())) {
        rtThrowTypeError("Converting circular structure to JSON");
        return false;
    }
    state.stack.push_back(value.slot_ptr());
    const Units stepback = state.indent;
    state.indent.insert(state.indent.end(), state.gap.begin(), state.gap.end());

    std::vector<Units> keys;
    bool failed = false;
    if (state.hasPropertyList) {
        keys = state.propertyList;
    } else {
        Rooted<Value> keyArray{Value(bronze_object_keys(value.get().rawBits()))};
        if (rtExceptionPending()) {
            failed = true;
        } else {
            const uint32_t count = keyArray.get().asObject<ArrayHeader>()->length;
            for (uint32_t i = 0; i < count; ++i) {
                Value k = keyArray.get().asObject<ArrayHeader>()->getElem(i);
                keys.push_back(rtStringUnits(k.asString<StringHeader>()));
            }
        }
    }

    std::vector<Units> partial;
    for (size_t i = 0; !failed && i < keys.size(); ++i) {
        Units serialized;
        if (!serializeProperty(state, keys[i], value, serialized)) {
            if (rtExceptionPending()) {
                failed = true;
                break;
            }
            continue;  // undefined: the member is not written at all
        }
        Units member;
        quoteJsonString(keys[i], member);
        member.push_back(u':');
        // The space after the colon exists only when the gap does, which is
        // 25.5.2.4 step 8.b.ii read literally and the reason `{"a":1}` and
        // `{\n  "a": 1\n}` differ by more than newlines.
        if (!state.gap.empty()) member.push_back(u' ');
        member.insert(member.end(), serialized.begin(), serialized.end());
        partial.push_back(std::move(member));
    }
    if (!failed) joinPartial(partial, state.indent, stepback, state.gap, u'{', u'}', out);

    state.stack.pop_back();
    state.indent = stepback;
    return !failed;
}

// 25.5.2.3 SerializeJSONProperty. False means "undefined" — the caller decides
// whether that is an omission (an object member) or a `null` (an array
// element), which is the whole difference between the two containers.
bool serializeProperty(State& state, const Units& key, Rooted<Value>& holder, Units& out) {
    Rooted<Value> keyString{rtStringFromUnits(key)};
    Rooted<Value> value{
        Value(bronze_elem_get(holder.get().rawBits(), keyString.get().rawBits()))};
    if (rtExceptionPending()) return false;

    // Step 2: `toJSON` is consulted on any object, which is how a class
    // chooses its own wire form. It is read with the ordinary property
    // machinery, so an inherited one is found.
    if (value.get().isObject()) {
        Rooted<Value> name{rtMakeString("toJSON")};
        Rooted<Value> toJson{
            Value(bronze_elem_get(value.get().rawBits(), name.get().rawBits()))};
        if (rtExceptionPending()) return false;
        if (isCallable(toJson.get())) {
            uint64_t args[1] = {keyString.get().rawBits()};
            value.set(Value(
                bronze_dynamic_call(toJson.get().rawBits(), value.get().rawBits(), 1, args)));
            if (rtExceptionPending()) return false;
        }
    }

    // Step 3: the replacer FUNCTION sees every key including the root's, whose
    // key is the empty string, and its receiver is the holder.
    if (state.replacer && isCallable(*state.replacer)) {
        uint64_t args[2] = {keyString.get().rawBits(), value.get().rawBits()};
        value.set(Value(bronze_dynamic_call(state.replacer->rawBits(), holder.get().rawBits(), 2,
                                            args)));
        if (rtExceptionPending()) return false;
    }

    const Value v = value.get();
    if (v.isNull()) {
        appendAscii(out, "null");
        return true;
    }
    if (v.isBool()) {
        appendAscii(out, v.asBool() ? "true" : "false");
        return true;
    }
    if (v.isString()) {
        quoteJsonString(rtStringUnits(v.asString<StringHeader>()), out);
        return true;
    }
    if (isNumberLike(v)) {
        const double d = numberOf(v);
        if (!std::isfinite(d)) {
            // NaN and both infinities have no JSON spelling, and `null` is
            // what 25.5.2.3 step 10 substitutes rather than failing. `-0`
            // needs no case of its own: ToString(-0) is already "0".
            appendAscii(out, "null");
            return true;
        }
        char buf[40];
        const size_t len = formatJsNumber(d, buf);
        appendAscii(out, std::string(buf, len));
        return true;
    }
    if (v.isObject() && !isCallable(v)) {
        if (isArray(v)) return serializeArray(state, value, out);
        if (v.asObject<HeapObjectHeader>()->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) {
            // A Map, a Set or a typed array has no own enumerable string
            // properties, so ECMA-262 serializes it as `{}` — a real answer
            // rather than a fallback, and the reason `JSON.stringify(map)`
            // is the classic surprise it is in every engine.
            appendAscii(out, "{}");
            return true;
        }
        return serializeObject(state, value, out);
    }
    return false;  // undefined, a function, or a symbol
}

}  // namespace

Value rtJsonStringify(Value value, Value replacer, Value space) {
    State state;
    Rooted<Value> valueRoot{value};
    Rooted<Value> replacerRoot{replacer};
    Rooted<Value> spaceRoot{space};

    if (isCallable(replacerRoot.get())) {
        state.replacer = replacerRoot.slot_ptr();
    } else if (isArray(replacerRoot.get())) {
        // 25.5.2.1 step 4.b: an ARRAY replacer is a property allow-list, in
        // its own order, with duplicates removed.
        state.hasPropertyList = true;
        const uint32_t count = replacerRoot.get().asObject<ArrayHeader>()->length;
        for (uint32_t i = 0; i < count; ++i) {
            Rooted<Value> item{replacerRoot.get().asObject<ArrayHeader>()->getElem(i)};
            if (!item.get().isString() && !isNumberLike(item.get())) continue;
            Rooted<Value> asString{rtValueToString(item.get())};
            Units key = rtStringUnits(asString.get().asString<StringHeader>());
            bool seen = false;
            for (const Units& existing : state.propertyList) {
                if (existing == key) seen = true;
            }
            if (!seen) state.propertyList.push_back(std::move(key));
        }
    }

    // 25.5.2.1 steps 6 and 7: a number gives that many spaces, capped at ten;
    // a string gives its first ten code units; anything else gives none.
    if (isNumberLike(spaceRoot.get())) {
        double n = numberOf(spaceRoot.get());
        if (std::isnan(n)) n = 0;
        if (n > 10) n = 10;
        for (int i = 0; i < static_cast<int>(n); ++i) state.gap.push_back(u' ');
    } else if (spaceRoot.get().isString()) {
        Units text = rtStringUnits(spaceRoot.get().asString<StringHeader>());
        if (text.size() > 10) text.resize(10);
        state.gap = std::move(text);
    }

    // The wrapper of step 9 is real rather than simulated: the replacer is
    // called with it as the receiver and with the empty key, and a replacer
    // that reads `this[""]` must see the value.
    Rooted<Value> wrapper{Value(bronze_create_object())};
    Rooted<Value> emptyKey{rtMakeString("")};
    bronze_elem_set(wrapper.get().rawBits(), emptyKey.get().rawBits(), valueRoot.get().rawBits());

    Units out;
    if (!serializeProperty(state, Units{}, wrapper, out)) {
        // `undefined` at the root stays `undefined` rather than becoming the
        // string "undefined": `JSON.stringify(undefined)` and
        // `JSON.stringify(function () {})` both answer the value, which is
        // what lets a caller test for it.
        return Value::fromUndefined();
    }
    return rtStringFromUnits(out);
}

}  // namespace bronze::runtime
