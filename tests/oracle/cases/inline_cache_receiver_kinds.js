// Arrays, typed arrays, ArrayBuffers, functions and strings all reach the
// property helper and are told apart by `flags` (or, for a string, by the value
// tag). The inlined fast path has to make the same distinctions before it
// believes a shape word, because on any of these the word at that offset is a
// length or a code pointer.
//
// Getting a non-plain-object through a site inference marked monomorphic
// takes a constructor that returns one: ECMA-262 9.2.2 says [[Construct]]
// yields the returned value when it is an Object, so `new Make...()` below
// evaluates to the array / view / function, while the compile-time shape
// class is still the `this.tag = ...` layout the constructor never gets to
// hand back. Every read on those receivers therefore enters the inline
// sequence and has to be turned away by the flag check.

function MakeArray() {
  this.tag = "unreachable";
  return [10, 20, 30];
}
const arr = new MakeArray();
console.log(arr.length);
console.log(arr[0]);
console.log(arr[2]);
console.log(arr.tag);

function MakeView() {
  this.tag = "unreachable";
  return new Float32Array(4);
}
const view = new MakeView();
console.log(view.length);
view[1] = 2.5;
console.log(view[1]);
console.log(view[3]);
console.log(view.buffer.byteLength);

// An ArrayBuffer of length zero is the sharpest corner of the flag check.
// Its byteLength and reserved fields are both zero, so the word the fast
// path would read as a shape is NULL — which is exactly what a cold cache
// entry holds. Drop the flag discrimination and this receiver "matches" an
// entry nothing ever filled, and the load runs off the end of an
// eight-byte payload.
function MakeBuf() {
  this.tag = "unreachable";
  return new ArrayBuffer(0);
}
function Neighbour(v) {
  this.v = v;
}
const emptyBuf = new MakeBuf();
// Allocated immediately after the empty buffer, so the bytes an unchecked
// fast path would read past its eight-byte payload belong to a live object
// instead of being untouched zeroes — the difference between a broken
// guard that shows and one that hides behind a fresh page.
const neighbour = new Neighbour(1234);
console.log(emptyBuf.byteLength);
console.log(neighbour.v);

function MakeFn() {
  this.tag = "unreachable";
  return function () {
    return 7;
  };
}
const fn = new MakeFn();
const fnProto = fn.prototype;
fnProto.marker = "on-the-prototype";
console.log(fn.prototype.marker);
console.log(fn());

// The same reads in a loop, so the answer comes from a filled cache entry
// rather than from a first-time walk.
let total = 0;
let i = 0;
while (i < 200) {
  total = total + arr.length + view.length;
  i = i + 1;
}
console.log(total);

// A string receiver never reaches the flag check at all: it fails the
// object-tag test first, because a string is not an Object-tagged value.
// No sound proof can route one into the inline form either — the form's
// licence is `Object` with a shape class — so this is the helper's path,
// which the ABI change moved the cache pointer through.
const holder = { s: "hello" };
console.log(holder.s.length);
console.log(holder.s.charCodeAt(1));

// And a plain object right after all of them, to show the shared site
// machinery still answers correctly for the case it is built for.
function Plain(v) {
  this.v = v;
}
const plain = new Plain(42);
console.log(plain.v);
