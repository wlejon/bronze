#pragma once

#include "runtime/heap.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

namespace bronze {

// A property key: ECMA-262 6.1.7's key type, which is a String or a Symbol and
// nothing else.
//
// One pointer wide, because both kinds begin with the same HeapObjectHeader and
// its `tag` already says which — so a shape node, a dictionary entry and an
// own-key vector are the size they always were. What this type buys is not
// storage. It is that the two MATCHING rules can no longer be applied to the
// wrong kind: a string key matches by CONTENT, which is what lets two objects
// with the same property names share a shape, and a symbol key matches by
// IDENTITY, because two symbols with the same description are two keys.
// `matches` below is the only place either rule is written.
//
// It is deliberately NOT implicitly convertible from a raw pointer in the
// storage direction: every site that used to say "the key" and mean "the
// interned string" has to say which it means now, and the sites that hand a key
// back to a PROGRAM — `Object.keys`, `for-in`, spread, JSON — have to decide
// what a symbol does there. Converting a `StringHeader*` for a LOOKUP is
// unambiguous and stays implicit, since asking for a string key can only ever
// mean one thing.
class PropertyKey {
public:
    constexpr PropertyKey() noexcept = default;

    // Looking a string key up is unambiguous, so this one direction converts
    // silently: `shape->lookupProperty(interned, info)` still reads as itself.
    PropertyKey(StringHeader* s) noexcept  // NOLINT(google-explicit-constructor)
        : ptr_(s ? &s->header : nullptr) {}

    static PropertyKey forString(StringHeader* s) noexcept { return PropertyKey(s); }

    static PropertyKey forSymbol(SymbolHeader* s) noexcept {
        return PropertyKey(s ? &s->header : nullptr);
    }

    // The result of ToPropertyKey (7.1.19), which is already a string or a
    // symbol. An invalid key for anything else — the caller has converted, or
    // has a bug.
    static PropertyKey fromValue(Value v) noexcept {
        if (v.isString()) return forString(v.asString<StringHeader>());
        if (v.isSymbol()) return forSymbol(v.asSymbol<SymbolHeader>());
        return PropertyKey();
    }

    bool valid() const noexcept { return ptr_ != nullptr; }

    bool isSymbol() const noexcept {
        return ptr_ && ptr_->tag == static_cast<uint16_t>(Tag::Symbol);
    }
    bool isString() const noexcept {
        return ptr_ && ptr_->tag == static_cast<uint16_t>(Tag::String);
    }

    // Null unless the key is of that kind, so a caller that forgot to ask
    // dereferences null rather than reading a symbol's fields as a string's.
    StringHeader* string() const noexcept {
        return isString() ? reinterpret_cast<StringHeader*>(ptr_) : nullptr;
    }
    SymbolHeader* symbol() const noexcept {
        return isSymbol() ? reinterpret_cast<SymbolHeader*>(ptr_) : nullptr;
    }

    Value toValue() const noexcept {
        if (isSymbol()) return Value::fromSymbol(ptr_);
        if (isString()) return Value::fromString(ptr_);
        return Value::fromUndefined();
    }

    // THE rule, and the reason this type exists. Same pointer is always the
    // same key; past that, two strings compare by content and everything else
    // — including two distinct symbols with identical descriptions — is a
    // different key.
    bool matches(PropertyKey other) const noexcept {
        if (!ptr_ || !other.ptr_) return false;
        if (ptr_ == other.ptr_) return true;
        if (!isString() || !other.isString()) return false;
        return reinterpret_cast<const StringHeader*>(ptr_)->equals(
            *reinterpret_cast<const StringHeader*>(other.ptr_));
    }

private:
    explicit constexpr PropertyKey(HeapObjectHeader* p) noexcept : ptr_(p) {}

    HeapObjectHeader* ptr_{nullptr};
};

}  // namespace bronze
