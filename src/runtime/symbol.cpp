// The Symbol primitive (ECMA-262 6.1.5) and the global registry of 20.4.2.
//
// A symbol is arena-allocated, and everything odd-looking here follows from
// that one decision (runtime/symbol.h explains why it is forced): nothing below
// needs a GC root, nothing below can move, and a symbol handed to a program is
// the same pointer forever — which is what makes it a property key.

#include "runtime/symbol.h"

#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/heap.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// The immortal, non-moving copy of a description. A symbol outlives every
// collection, so a heap string in it would dangle — the same rule a shape key
// follows, and reached the same way.
StringHeader* internDescription(Value description) {
    if (description.isUndefined()) return nullptr;
    if (!description.isString()) {
        fatal("internal: a symbol description that is neither a string nor undefined");
    }
    return StringHeader::internToArena(rtArena(), description.asString<StringHeader>());
}

SymbolHeader* allocateSymbol(StringHeader* description) {
    void* mem = rtArena().allocate(sizeof(SymbolHeader), alignof(SymbolHeader));
    auto* sym = static_cast<SymbolHeader*>(mem);
    // The tag is what `PropertyKey` discriminates on, so it is set here and
    // never anywhere else. `size` is filled in for the benefit of a debugger
    // and of nothing else: the collector never sees this block, because
    // `Heap::forward_value` ignores every pointer outside its semispace.
    sym->header.tag = static_cast<uint16_t>(Tag::Symbol);
    sym->header.flags = 0;
    sym->header.size = static_cast<uint32_t>(sizeof(SymbolHeader));
    sym->description = description;
    return sym;
}

// The `Symbol.for` registry (20.4.2.1's [[GlobalSymbolRegistry]]).
//
// A VECTOR and not a hash map, and the reason is the determinism rule rather
// than the size: `Symbol.keyFor` walks it, `getOwnPropertySymbols` orders
// against creation, and a bucket order reaching either would put an address in
// an output path. It is searched linearly because a program's registered
// symbols are counted in ones and twos.
//
// It needs no GC root: every key string and every symbol in it lives in the
// arena, which the collector does not walk.
struct RegistryEntry {
    StringHeader* key;  // arena-interned
    SymbolHeader* symbol;
};

std::vector<RegistryEntry>& registry() {
    static std::vector<RegistryEntry> entries;
    return entries;
}

}  // namespace

Value rtMakeSymbol(Value description) {
    return Value::fromSymbol(allocateSymbol(internDescription(description)));
}

namespace {

// A well-known symbol, built on FIRST USE and not at load time: interning the
// description goes through the heap, so this must not run before the heap
// exists. The earliest anything can ask for one is a program evaluating
// `Symbol.iterator` or opening an iteration, both of which are long past that
// point.
SymbolHeader* wellKnownSymbol(std::string_view description) {
    return allocateSymbol(StringHeader::internToArena(
        rtArena(), StringHeader::createFromUTF8(rtHeap(), description)));
}

}  // namespace

SymbolHeader* rtSymbolIterator() {
    static SymbolHeader* sym = wellKnownSymbol("Symbol.iterator");
    return sym;
}

SymbolHeader* rtSymbolToStringTag() {
    static SymbolHeader* sym = wellKnownSymbol("Symbol.toStringTag");
    return sym;
}

Value rtSymbolFor(Rooted<Value>& keyString) {
    if (!keyString.get().isString()) {
        fatal("internal: Symbol.for with a key that is not a string");
    }
    StringHeader* wanted = keyString.get().asString<StringHeader>();
    for (const RegistryEntry& e : registry()) {
        if (e.key->equals(*wanted)) return Value::fromSymbol(e.symbol);
    }
    // 20.4.2.1 step 4: the registered symbol's [[Description]] IS the key, so
    // one interned copy serves as both.
    StringHeader* interned = StringHeader::internToArena(rtArena(), wanted);
    SymbolHeader* fresh = allocateSymbol(interned);
    registry().push_back(RegistryEntry{interned, fresh});
    return Value::fromSymbol(fresh);
}

Value rtSymbolKeyFor(Value symbol) {
    if (!symbol.isSymbol()) return Value::fromUndefined();
    auto* sym = symbol.asSymbol<SymbolHeader>();
    for (const RegistryEntry& e : registry()) {
        // By IDENTITY, which is the whole content of the question: a symbol
        // whose description happens to equal a registered key is not in the
        // registry, and `Symbol.keyFor(Symbol("shared"))` is `undefined` even
        // when `Symbol.for("shared")` has been called.
        if (e.symbol == sym) return rtCopyKeyToHeap(e.key);
    }
    return Value::fromUndefined();
}

std::string rtSymbolDescriptiveString(Value symbol) {
    if (!symbol.isSymbol()) fatal("internal: a descriptive string for a value that is not a symbol");
    const StringHeader* desc = symbol.asSymbol<SymbolHeader>()->description;
    // 20.4.3.3.1: an ABSENT description reads as the empty string here, so
    // `Symbol()` and `Symbol("")` both print `Symbol()`. `.description` is what
    // distinguishes them, and deliberately so.
    return "Symbol(" + (desc ? rtUtf8Chars(desc) : std::string()) + ")";
}

namespace {

uint64_t symbolProtoToString(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Value self(thisBits);
    if (!self.isSymbol()) {
        return rtThrowTypeError("Symbol.prototype.toString called on an incompatible receiver")
            .rawBits();
    }
    return rtMakeString(rtSymbolDescriptiveString(self)).rawBits();
}

// 20.4.3.4 thisSymbolValue, which for a PRIMITIVE receiver is the receiver.
// bronze has no Symbol wrapper object, so the other half of 20.4.3.4 — a
// [[SymbolData]] slot to unwrap — has nothing to answer for and is not missing.
uint64_t symbolProtoValueOf(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Value self(thisBits);
    if (!self.isSymbol()) {
        return rtThrowTypeError("Symbol.prototype.valueOf called on an incompatible receiver")
            .rawBits();
    }
    return thisBits;
}

// 20.4.3 members bronze has not built.
const char* const kSymbolProtoUnimplemented[] = {
    "toLocaleString",
};

}  // namespace

Value rtSymbolMember(Value symbol, const std::string& key) {
    if (key == "description") {
        const StringHeader* desc = symbol.asSymbol<SymbolHeader>()->description;
        // `undefined`, not "", for a symbol made without one — 20.4.3.2 reads
        // [[Description]] straight out and that field is genuinely absent.
        if (!desc) return Value::fromUndefined();
        return rtCopyKeyToHeap(desc);
    }
    if (key == "toString") {
        return rtNativeFunction(symbolProtoToString, 0);
    }
    if (key == "valueOf") {
        return rtNativeFunction(symbolProtoValueOf, 0);
    }
    // 20.4.3.1's back-pointer, as the same object the bare name `Symbol`
    // resolves to. A primitive has no chain here to find it on, which is why
    // this is a branch rather than a property.
    if (key == "constructor") return rtSymbolFunction();
    rtCheckUnimplementedMember("Symbol.prototype", kSymbolProtoUnimplemented,
                               std::size(kSymbolProtoUnimplemented), key);
    return Value::fromUndefined();
}

}  // namespace bronze::runtime
