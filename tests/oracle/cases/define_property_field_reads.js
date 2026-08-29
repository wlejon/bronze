// How `Object.defineProperty` READS its descriptor: 6.2.6.5
// ToPropertyDescriptor, field by field.
//
// Each of the six fields is asked twice — HasProperty, then Get — and the pair
// is what this case pins, because the two questions have different answers and
// a decode that collapsed them would be wrong in both directions. HasProperty
// walks the PROTOTYPE CHAIN, so a field the descriptor inherits counts; and it
// is presence and not truthiness, so `{ value: undefined }` MENTIONS value and
// sets it, where `{}` mentions nothing and leaves an existing property alone.
//
// The distinction is not academic in bronze: the decode answers both questions
// from one walk of the chain wherever the answer is a data slot, and falls back
// to the two generic calls wherever it is not — an accessor field, whose Get
// runs user code and must run it exactly once, or a receiver whose keys do not
// all live in a shape. Every one of those roads is below.
//
// `Object.prototype` is polluted at the end, deliberately: it is the last
// object on a descriptor's chain, so a field name written there is visible to
// every descriptor in the program, and a walk that stopped short of it would
// report the field absent.

function show(label, o, k) {
  const d = Object.getOwnPropertyDescriptor(o, k);
  if (d === undefined) { console.log(label + ": absent"); return; }
  if ('get' in d || 'set' in d) {
    console.log(label + ": get=" + typeof d.get + " set=" + typeof d.set +
                " enumerable=" + d.enumerable + " configurable=" + d.configurable);
  } else {
    console.log(label + ": value=" + String(d.value) + " writable=" + d.writable +
                " enumerable=" + d.enumerable + " configurable=" + d.configurable);
  }
}

// A new property takes false for every attribute the descriptor is silent
// about (10.1.6.3 step 3, through CompletePropertyDescriptor).
const o1 = {};
Object.defineProperty(o1, 'a', { value: 1, writable: true });
show("own", o1, 'a');

// An EXISTING property keeps the attributes the descriptor is silent about
// (step 4 sets only the fields it has), so `value: undefined` and no `value`
// at all are two different edits to the same property.
const o2 = {};
Object.defineProperty(o2, 'b', { value: 1, writable: true, enumerable: true, configurable: true });
Object.defineProperty(o2, 'b', { value: undefined });
show("undefined-value", o2, 'b');
const o2b = {};
Object.defineProperty(o2b, 'b', { value: 1, writable: true, enumerable: true, configurable: true });
Object.defineProperty(o2b, 'b', {});
show("no-value", o2b, 'b');

// HasProperty, so an inherited field is a mentioned field.
const proto3 = { enumerable: true };
const d3 = Object.create(proto3);
d3.value = 7;
const o3 = {};
Object.defineProperty(o3, 'c', d3);
show("inherited", o3, 'c');

// Two links up is no different from one.
const base4 = { writable: true };
const mid4 = Object.create(base4);
const top4 = Object.create(mid4);
top4.value = 3;
const o4 = {};
Object.defineProperty(o4, 'd', top4);
show("deep", o4, 'd');

// A field that is an ACCESSOR on the descriptor. HasProperty does not run a
// getter and Get runs it once, so each of these counts exactly one call — and
// the value the property ends up with is what the getter returned, never the
// getter itself.
let nValue = 0, nWritable = 0, nEnumerable = 0;
const d5 = {};
Object.defineProperty(d5, 'value',
  { get: function () { nValue++; return 42; }, configurable: true });
Object.defineProperty(d5, 'writable',
  { get: function () { nWritable++; return true; }, configurable: true });
Object.defineProperty(d5, 'enumerable',
  { get: function () { nEnumerable++; return true; }, configurable: true });
const o5 = {};
Object.defineProperty(o5, 'e', d5);
show("accessor-fields", o5, 'e');
console.log("getter calls: value=" + nValue + " writable=" + nWritable +
            " enumerable=" + nEnumerable);

// The same accessor, inherited rather than own.
let nInherited = 0;
const proto6 = {};
Object.defineProperty(proto6, 'value',
  { get: function () { nInherited++; return 'inh'; }, configurable: true });
const d6 = Object.create(proto6);
const o6 = {};
Object.defineProperty(o6, 'f', d6);
show("inherited-accessor", o6, 'f');
console.log("inherited getter calls: " + nInherited);

// A descriptor whose own keys have left shape-land: `delete` puts an object
// into dictionary mode, and the fields still read the same.
const d7 = { value: 11, writable: true, junk: 2 };
delete d7.junk;
const o7 = {};
Object.defineProperty(o7, 'g', d7);
show("dictionary", o7, 'g');

// 6.2.6.5 takes any object, and a String wrapper is one — its own `length` and
// index keys are answered beside its shape, which is why it is not a receiver
// a shape walk alone describes.
const d8 = new String('ab');
d8.value = 9;
const o8 = {};
Object.defineProperty(o8, 'h', d8);
show("string-wrapper", o8, 'h');

// The read side of an accessor the descriptor DEFINES, as against one it is
// spelled with.
const o9 = {};
Object.defineProperty(o9, 'i', {
  get: function () { return 5; },
  enumerable: true,
  configurable: true
});
show("defines-accessor", o9, 'i');
console.log("read: " + o9.i);

// `defineProperties` is a loop over the same decode (20.1.2.3.1 step 4), so
// each descriptor in the map is read exactly as a lone one is.
const o10 = {};
Object.defineProperties(o10, {
  j: { value: 1, enumerable: true, configurable: true },
  k: { value: 2 }
});
show("defineProperties j", o10, 'j');
show("defineProperties k", o10, 'k');

// The end of every descriptor's chain. While `configurable` is there, every
// descriptor in the program mentions it.
Object.prototype.configurable = true;
const o12 = {};
Object.defineProperty(o12, 'm', { value: 4 });
show("proto-pollution", o12, 'm');
delete Object.prototype.configurable;
const o12b = {};
Object.defineProperty(o12b, 'm', { value: 4 });
show("after-cleanup", o12b, 'm');
