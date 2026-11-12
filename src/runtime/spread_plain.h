#pragma once

// The copy `{ ...src }` and a rest pattern make when both ends are plain
// (spread_plain.cpp).

#include "runtime/gc.h"
#include "runtime/value.h"

namespace bronze::runtime {

// CopyDataProperties (7.3.25) of `src` into `target` for the case that needs
// no key list, no key lookup on the source and no user code: `src` a plain
// object on the transition tree whose own properties are all data properties
// under non-index string names, `target` a plain extensible one. `excluded` is
// a rest pattern's key array (strings and symbols) or null. False when the
// case is not this one, and then nothing has been written.
bool rtSpreadPlain(Rooted<Value>& target, Rooted<Value>& src, const Rooted<Value>* excluded);

// With the shape lookup memos (Shape::forgetLookupMemo): the hints below name
// shapes of the arena that is going away.
void rtForgetSpreadShapeHints() noexcept;

}  // namespace bronze::runtime
