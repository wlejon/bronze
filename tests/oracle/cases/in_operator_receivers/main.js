// The `in` operator (ECMA-262 13.10.2) against EVERY receiver kind bronze has
// a heap object for, which is the case that did not exist when
// `'size' in new Map()` was dereferencing a `Value` as a `Shape*` and killing
// the process with no diagnostic.
//
// Every byte of `main.expected` was derived by hand from ECMA-262 before this
// program was ever compiled: 13.10.2 step 5 for the primitive right-hand side,
// 7.3.11 HasProperty and 10.1.7.1 OrdinaryHasProperty for the chain walk, and
// then the clause that owns each kind — 10.4.2 for an array, 10.4.5 for a typed
// array, 25.1.6 and 25.3.4 for an ArrayBuffer and a DataView, 10.2.4 for a
// function, 24.1.3 and 24.2.3 for a Map and a Set, 22.2.6 for a RegExp, and
// 10.4.6.4 for a module namespace.
//
// What makes this one case rather than eleven: `in` is ONE operator with one
// dispatch behind it, and the bug was that the dispatch had a fall-through. A
// kind tested in its own file is a kind whose arm can go missing without any
// file noticing.
import * as ns from './lib.js';

// ---- a plain object, and the prototype chain the walk continues onto --------
// A property whose VALUE is undefined still answers true: 7.3.11 asks whether
// the property is there and never reads it, which is the whole difference
// between `in` and a property read.
const proto = { inherited: 1 };
const plain = Object.create(proto);
plain.own = 1;
plain.undef = undefined;
console.log('own' in plain, 'undef' in plain, 'inherited' in plain, 'missing' in plain);

// ---- an array: an index, a hole, `length`, and one past the end -------------
// 10.4.2. A hole left by `delete` stops being an own key without `length`
// moving, which is the pair of answers `in` exists on an array to give.
const arr = ['a', 'b', 'c'];
delete arr[1];
console.log(0 in arr, 1 in arr, 2 in arr, 3 in arr, 'length' in arr);

// ---- a typed array ---------------------------------------------------------
// 10.4.5.2: an index outside the range is ABSENT rather than inherited, so
// there is no member table for it to fall through to and no chain above it.
const ta = new Uint8Array(2);
console.log(0 in ta, 1 in ta, 2 in ta, -1 in ta);
console.log('length' in ta, 'byteLength' in ta, 'byteOffset' in ta, 'buffer' in ta,
            'BYTES_PER_ELEMENT' in ta);
console.log('map' in ta, 'missing' in ta);

// ---- an ArrayBuffer and a DataView ------------------------------------------
// Both keep every member on a prototype bronze has not built as an object, so
// `in` has to ask the same table the property reads come from — reporting the
// object empty would contradict `buf.byteLength` answering 8.
const buf = new ArrayBuffer(8);
console.log('byteLength' in buf, 'constructor' in buf, 'missing' in buf);

const view = new DataView(buf);
console.log('byteLength' in view, 'byteOffset' in view, 'buffer' in view);
console.log('getInt8' in view, 'setFloat64' in view, 'missing' in view);

// ---- a function -------------------------------------------------------------
// `prototype` is an own property of every ordinary function (10.2.4) whether or
// not anything has read it yet, and a static is an own property like any other.
function f(a) {
  return a;
}
f.stat = 1;
console.log('prototype' in f, 'stat' in f, 'missing' in f);

// ---- a Map ------------------------------------------------------------------
// 24.1.3. An ENTRY is not a property: it lives in [[MapData]], which no
// property key names — so a key that is in the map is still absent to `in`,
// and the read below proves the entry is really there.
const m = new Map();
m.set('missing', 'an entry, not a property');
console.log('size' in m, 'get' in m, 'set' in m, 'has' in m, 'delete' in m);
console.log('clear' in m, 'forEach' in m, 'keys' in m, 'values' in m, 'entries' in m);
console.log('missing' in m, m.get('missing'));

// ---- a Set ------------------------------------------------------------------
// 24.2.3, which is not 24.1.3 with a different name: `add` is a Set's and
// `get`/`set` are a Map's, and answering from one table for both receivers is
// the mistake this line is here to catch.
const s = new Set([1]);
console.log('size' in s, 'add' in s, 'has' in s, 'delete' in s, 'clear' in s);
console.log('forEach' in s, 'keys' in s, 'values' in s, 'entries' in s);
console.log('get' in s, 'set' in s, 'missing' in s);

// ---- a RegExp ---------------------------------------------------------------
// 22.2.6. `lastIndex` is the one OWN property; `source`, `flags` and the six
// flag names are prototype accessors, and `in` cannot tell the two apart —
// which is correct, because 7.3.11 does not either.
const re = /a(b)c/gi;
console.log('source' in re, 'flags' in re, 'lastIndex' in re);
console.log('global' in re, 'ignoreCase' in re, 'multiline' in re, 'dotAll' in re,
            'unicode' in re, 'sticky' in re);
console.log('exec' in re, 'test' in re, 'toString' in re, 'missing' in re);

// ---- a module namespace -----------------------------------------------------
// 10.4.6.4, which is the whole question: [[Prototype]] is null (10.4.6.1), so
// an export name is true and NOTHING else can be.
console.log('live' in ns, 'fixed' in ns, 'bump' in ns, 'missing' in ns);

// ---- a SYMBOL key, which never goes through ToString -------------------------
// ToString of a symbol is a TypeError, so `sym in o` has to be answered before
// any key conversion. On a receiver with a shape it is an ordinary own-key
// question; on one without, the only symbol that can be there is a well-known
// one, and @@iterator is on four of these prototypes and none of the other
// four.
const tag = Symbol('tag');
const holder = {};
holder[tag] = 1;
console.log(tag in holder, Symbol('other') in holder);
console.log(Symbol.iterator in arr, Symbol.iterator in ta, Symbol.iterator in m,
            Symbol.iterator in s);
console.log(Symbol.iterator in re, Symbol.iterator in buf, Symbol.iterator in view,
            Symbol.iterator in ns);

// ---- the right-hand side that is not an object -------------------------------
// 13.10.2 step 5: a primitive throws rather than answering false, which is what
// makes `in` unusable as a guard on an unknown value and `?.` the thing to
// reach for instead.
try {
  console.log('a' in 'abc');
} catch (e) {
  console.log(e instanceof TypeError);
}
