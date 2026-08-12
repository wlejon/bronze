// BLOCKED, and it is a SEGMENTATION FAULT rather than a wrong answer:
// `console.log(m)` on a Map or a Set exits 139 having printed nothing.
//
// `inspect.cpp`'s format() switches on the heap tag and its `default:` arm
// does `object(reinterpret_cast<ObjectHeader*>(hdr), depth)`. A MapHeader is
// not an ObjectHeader and has no `shape` word where that cast expects one, so
// the read is of whatever the allocation happens to hold. Every Map, every
// Set, and every iterator object taken from one goes down that arm.
//
// This is the worst failure shape the house rules recognise. "Hard errors over
// silent fallbacks" exists so that a construct bronze has not built says so by
// name; a crash says nothing at all, and says it after the process is past
// recovering. It ranks above the missing feature it is attached to.
//
// It survived because nothing reached it. `cases/map_and_set.js` is a thorough
// case — 40-odd assertions across key identity, NaN keys, -0 normalisation and
// insertion order — and every one of them prints `m.size`, `m.get(k)` or
// `m.has(k)`. Not one prints the collection. The suite proved the data
// structure and never once looked at it.
//
// The expectation below is derived the way every other print format in this
// repo is: node's, which `cases/print_containers.js` already pins for arrays,
// objects, circular references and typed arrays. `Float32Array(3) [ 0, 0, 0 ]`
// there is the precedent for the `Ctor(size)` prefix, and the single-quoted
// string and the spaces inside the braces are that case's rules too. A Map's
// entries carry ` => ` between key and value, which is the one piece of
// spelling a Map adds; a Set prints its elements as a list because it has no
// second half to separate.
//
// The fix is a case arm per collection tag, not a guard on the default: a
// `default:` that casts anything it has not been taught is the bug, and the
// next heap kind added would land in it the same way.

const m = new Map();
m.set('a', 1);
m.set('b', 2);
console.log(m);
console.log(new Map());

const s = new Set();
s.add(1);
s.add(2);
console.log(s);
console.log(new Set());
