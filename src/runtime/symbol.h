#pragma once

#include <cstdint>
#include <string>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze {

// A Symbol (ECMA-262 6.1.5): a primitive whose identity IS its address, so two
// symbols with the same description are two different values and no amount of
// content comparison can make them one.
//
// It lives in the NON-MOVING ARENA rather than on the collected heap, and that
// single decision is what makes a symbol usable as a property key. A shape node
// is immortal and may never point into the movable heap — which is why a STRING
// key is copied into the arena on its way into a transition. Copying is
// available to a string precisely because a string key is matched by CONTENT; a
// copy of a symbol would be a different symbol, matched by nothing. So the
// symbol goes where the shape can point at it, and stays there.
//
// Two consequences worth stating rather than discovering. A symbol is never
// collected — the same bargain every property key in bronze already makes
// (`StringHeader::internToArena`), and symbols are created by the handful, not
// per iteration. And a Symbol-tagged Value needs no GC rooting of its own:
// `Heap::forward_value` skips any pointer outside the semispace, so the
// registry below is a plain table rather than a root source.
struct SymbolHeader {
    HeapObjectHeader header;
    // Arena-interned, or null for `Symbol()` with no argument. Null and the
    // empty string are DIFFERENT: `Symbol().description` is `undefined` and
    // `Symbol("").description` is `""`.
    //
    // It is the ONLY field, and nothing else may be added that a program can
    // observe: everything else about a symbol is its address. In particular
    // there is no creation ordinal, because nothing orders symbols — 6.1.7.1
    // puts an object's symbol keys in the order they were added to THAT object,
    // which is the transition chain's order and not the symbols' own.
    StringHeader* description;
};

}  // namespace bronze

namespace bronze::runtime {

// `Symbol()` / `Symbol(description)`: a fresh symbol every call, which is the
// whole point of the type. `description` must be a string Value or undefined;
// ToString of anything else is the caller's job (20.4.1.1 step 2).
Value rtMakeSymbol(Value description);

// `Symbol.for(key)` / `Symbol.keyFor(sym)` — the global registry of 20.4.2.1
// and 20.4.2.2, which returns the SAME symbol for the same string where
// `Symbol()` never does. `rtSymbolKeyFor` answers `undefined` for a symbol that
// is not in the registry.
Value rtSymbolFor(Rooted<Value>& keyString);
Value rtSymbolKeyFor(Value symbol);

// `Symbol.iterator` (ECMA-262 6.1.5.1 / 20.4.2.5) and `Symbol.toStringTag`
// (20.4.2.14): the two well-known symbols bronze has, each the one interned
// identity every reader and every writer of its hook must agree on. The
// [[Description]] is the symbol's own name, so `String(Symbol.iterator)` is
// "Symbol(Symbol.iterator)".
//
// A well-known symbol is a permanent root by construction rather than by
// registration: it lives in the arena like every other symbol, the collector
// never walks the arena, and the `static` that holds it therefore cannot go
// stale (runtime/symbol.h's opening note). Nothing about GC changes by adding
// one, which is exactly why the arena was the right home for symbols.
//
// The other eleven names 20.4.2 defines are NOT here and are not stand-ins
// either: they are in `kSymbolUnimplemented` (builtin_symbol.cpp), so
// `Symbol.asyncIterator` is a diagnosed missing member rather than
// `undefined`. These two exist because the hooks they name are built — the
// iterator protocol, and 20.1.3.6's tag lookup.
SymbolHeader* rtSymbolIterator();
SymbolHeader* rtSymbolToStringTag();
SymbolHeader* rtSymbolToPrimitive();

// SymbolDescriptiveString (20.4.3.3.1): `Symbol(desc)`, with an EMPTY
// description spelled `Symbol()` — the description of a `Symbol()` and of a
// `Symbol("")` print alike, which is exactly what the specification says and is
// why `.description` exists to tell them apart.
std::string rtSymbolDescriptiveString(Value symbol);

// `Symbol` itself — a function object, so `Symbol("tag")` is a call rather than
// "an object is not callable", and so its statics reach the function property
// path.
Value rtSymbolFunction();
void rtSymbolCheckMissingMember(Value fn, const std::string& key);

// `Symbol.prototype` (20.4.3): a real object on the real chain, which a
// primitive symbol reaches by the ordinary prototype walk. Unlike the other
// three prototypes it is an ORDINARY object and not a wrapper — 20.4.3 says so
// in as many words ("it is not a Symbol instance and does not have a
// [[SymbolData]] internal slot"), which is why it is built here rather than
// beside the wrappers.
Value rtSymbolPrototype();

}  // namespace bronze::runtime
