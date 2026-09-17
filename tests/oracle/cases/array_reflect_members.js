// The reflective `Object` members over an ARRAY receiver. An array is an
// object (20.1.2.x step 1 is only "If O is not an Object, throw"), so every
// one of these proceeds and reaches 10.4.2's own [[GetOwnProperty]] and
// [[DefineOwnProperty]]: an element is a data property with all three
// attributes true, `length` is writable and neither enumerable nor
// configurable, and everything else is an ordinary property. bronze keeps
// the first two outside any shape and the rest in a side object, so each
// member below has an array arm of its own — what this case pins is that
// the three stories answer as one object.
//
// What is deliberately absent, because bronze refuses it BY NAME rather than
// answering differently: an accessor at an index, an element with an attribute
// its neighbours lack (a non-enumerable index, a `length` made non-writable
// outside `Object.freeze`), and a new index whose descriptor leaves an
// attribute to default to false. Each is per-element storage the dense block
// does not keep.

function show(label, v) { console.log(label, JSON.stringify(v)); }
function attempt(fn) {
    try { return fn(); } catch (e) { return e.name; }
}

// --- describing ---------------------------------------------------------
const arr = [10, 20, 30];
arr.name = 'n';
show('gopd index', Object.getOwnPropertyDescriptor(arr, '1'));
show('gopd length', Object.getOwnPropertyDescriptor(arr, 'length'));
show('gopd named', Object.getOwnPropertyDescriptor(arr, 'name'));
show('gopd absent', [Object.getOwnPropertyDescriptor(arr, '3'),
                     Object.getOwnPropertyDescriptor(arr, 'other')]);
// A hole is not an own property: the descriptor is undefined and the key is
// absent from every listing.
delete arr[0];
show('gopd hole', Object.getOwnPropertyDescriptor(arr, '0'));
show('names', Object.getOwnPropertyNames(arr));
show('keys', Object.keys(arr));
show('descriptors', Object.getOwnPropertyDescriptors(arr));
show('hasOwn', ['0', '1', '3', 'length', 'name', 'other'].map((k) => Object.hasOwn(arr, k)));

// --- symbols live beside the named properties ----------------------------
const s = Symbol('s');
arr[s] = 'sym';
show('symbols', [Object.getOwnPropertySymbols(arr).length, Object.hasOwn(arr, s),
                 Object.getOwnPropertyDescriptor(arr, s)]);

// --- defining -----------------------------------------------------------
const a = [1, 2, 3];
// 10.4.2.1 step 2: an existing index with a value-only descriptor is a
// redefinition that leaves the attributes as CreateDataProperty made them.
Object.defineProperty(a, '1', { value: 99 });
show('define index', [a, Object.getOwnPropertyDescriptor(a, '1')]);
// A new index at `length`, with every attribute named, is an append.
Object.defineProperty(a, '3', { value: 4, writable: true, enumerable: true, configurable: true });
show('define append', [a, a.length]);
// A named key completes to all-false like any ordinary property.
Object.defineProperty(a, 'tag', { value: 't' });
show('define named', [a.tag, Object.keys(a), Object.getOwnPropertyDescriptor(a, 'tag')]);
// An accessor on a named key, with the array as `this`.
Object.defineProperty(a, 'first', { get() { return this[0]; }, configurable: true });
show('define accessor', [a.first, typeof Object.getOwnPropertyDescriptor(a, 'first').get]);
// `length` through a descriptor is ArraySetLength: it shrinks, it converts,
// and a value that is not an array length is the RangeError.
Object.defineProperty(a, 'length', { value: 2 });
show('define length', [a, a.length]);
Object.defineProperty(a, 'length', { value: '3' });
show('define length string', [a.length, 2 in a]);
show('define length bad', attempt(() => Object.defineProperty(a, 'length', { value: -1 })));
// `length` refuses what a non-configurable property refuses, and
// `Reflect.defineProperty` answers the same refusal as false.
show('define length attrs', [attempt(() => Object.defineProperty(a, 'length', { configurable: true })),
                             attempt(() => Object.defineProperty(a, 'length', { enumerable: true })),
                             attempt(() => Object.defineProperty(a, 'length', { get() { return 0; } })),
                             Reflect.defineProperty(a, 'length', { configurable: true })]);
show('reflect define', [Reflect.defineProperty(a, '0', { value: 'r' }), a[0]]);

// defineProperties: an index and a name in one batch.
const b = [10, 20, 30];
Object.defineProperties(b, {
    1: { value: 99 },
    name: { value: 'n' },
});
show('defineProperties', [b, b.length, b.name,
                          Object.getOwnPropertyDescriptor(b, '1'),
                          Object.getOwnPropertyDescriptor(b, 'name')]);

// --- integrity levels through the same members ---------------------------
const f = [1, 2];
f.n = 1;
Object.freeze(f);
show('frozen describe', [Object.getOwnPropertyDescriptor(f, '0'),
                         Object.getOwnPropertyDescriptor(f, 'length'),
                         Object.getOwnPropertyDescriptor(f, 'n')]);
// 10.1.6.3 step 4.e: a frozen element accepts the redefinition that changes
// nothing and refuses the one that does; a new key of any kind is refused
// because the array is not extensible.
show('frozen define', [Reflect.defineProperty(f, '0', { value: 1 }),
                       Reflect.defineProperty(f, '0', { value: 2 }),
                       Reflect.defineProperty(f, '2', { value: 3, writable: true, enumerable: true, configurable: true }),
                       Reflect.defineProperty(f, 'length', { value: 0 }),
                       Reflect.defineProperty(f, 'm', { value: 1 }),
                       attempt(() => Object.defineProperty(f, '0', { value: 2 })),
                       f, f.length]);
const sealed = [1, 2, 3];
Object.seal(sealed);
Object.defineProperty(sealed, '0', { value: 5 });
show('sealed define', [sealed, Object.getOwnPropertyDescriptor(sealed, '0'),
                       Reflect.defineProperty(sealed, 'length', { value: 1 }), sealed.length]);

// --- Object.assign with an array target ---------------------------------
show('assign', [Object.assign([1, 2], { 0: 'a', x: 1 }),
                Object.assign([], [7, 8], { length: 5 }).length]);
