#pragma once

#include "il/il.h"

namespace bronze::lower {

// REPEATED ELEMENT READS: `listeners[i].fn !== fn || ... listeners[i].context
// !== context` then `events.push(listeners[i])` reads one element of one Array
// three times an iteration, each a bounds test, the block's address, a load
// and a hole test. A read of an Array's element (an `elem.get` flagged
// kElemFlagArrayReceiver, il_pin.h) that an earlier read of the same receiver
// and index reaches, with nothing between them that can store an element or
// run code, is that earlier read.
//
// What stands between two such reads is, in a loop over objects, a named read
// of the element: `prop.get` with a static slot claim, whose hit is a load and
// whose miss is the ordinary access, which may run a getter. The miss is where
// the element may have changed, so the miss is where it is read again:
//
//       %t = is.shape %e            ; the read's own claim, tested here
//       br %t, hit, miss
//   hit:  %h = prop.get %e [shape-guarded]        ; a load
//         jump join(%h, %e)
//   miss: %m = prop.get %e                        ; the site as it was
//         %e2 = elem.get %a, %i                   ; the element, again
//         jump join(%m, %e2)
//   join(%v, %elem): ...
//
// The hit path is the guard and the load the site already was, and the later
// read of `%a[%i]` is `%elem`. The miss keeps the site's cell (only a site's
// own miss fills it, and the test asks that cell).
//
// Anything else between the reads that can store or run code (a call, an
// unclaimed read, a store) ends the element's availability; where the block
// that did so is left with the element still wanted on some path, it reads it
// again at its end, so the paths that did nothing of the kind keep theirs. A
// block entered by paths that hold different names for the element takes a
// parameter for it. A loop header and a handler start with nothing.
//
// A read made again sooner than the program made it (the miss arm's, a
// block's end) is a load of an element in bounds and not a hole. For a hole,
// or an index past the length, it is a lookup up the prototype chain, which
// runs code only where an index-named accessor or a Proxy stands on an
// Array's chain: such a getter may now run once where the program read the
// element twice, or on a path that would not have read it again.
//
// Last of the final passes, after the truth joins are threaded away (a join
// is where two names for one element would meet), and so, like that
// threading, in no build a profile is recorded from or probed against.
// Returns whether any function changed.
bool eliminateRepeatedElementReads(il::Module& module);

}  // namespace bronze::lower
