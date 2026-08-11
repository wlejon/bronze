// The inline cache lives in the generated object file now (docs/0010
// decision 7): a site inference proves monomorphic loads the receiver's
// shape word and the cached slot inline, and calls the helper only when
// that guard fails. These are the cases the guard has to survive.

// 1. A genuinely monomorphic site, hit until the CACHE rather than the
//    first walk is what answers.
function Point(x, y) {
  this.x = x;
  this.y = y;
}
const p = new Point(3, 4);
let sum = 0;
let i = 0;
while (i < 1000) {
  sum = sum + p.x + p.y;
  i = i + 1;
}
console.log(sum);

// 2. One PROVEN shape class, two runtime shapes. A shape class collects
//    every `this.k = ...` in the constructor, including the ones inside a
//    branch, so `Flag{on, extra}` names a layout `new Flag(false)` never
//    builds. That over-approximation is sound only because the emitted
//    guard compares the real shape word — delete the guard and the second
//    object reads the first one's cached slot.
function Flag(on) {
  this.on = on;
  if (on) {
    this.extra = "yes";
  }
}
const withExtra = new Flag(true);
const without = new Flag(false);
console.log(withExtra.on);
console.log(withExtra.extra);
console.log(without.on);
console.log(without.extra);

// 3. ONE site alternating between those two shapes, so the guard is
//    exercised on every call rather than once. `readOn` is direct-callable
//    and both arguments are `new Flag(...)`, so its parameter carries a
//    single shape class and the site is proven monomorphic — while the
//    shapes reaching it at run time are not.
function readOn(o) {
  return o.on;
}
let alternating = 0;
let k = 0;
while (k < 100) {
  if (readOn(withExtra)) {
    alternating = alternating + 1;
  }
  if (readOn(without)) {
    alternating = alternating + 10;
  }
  k = k + 1;
}
console.log(alternating);

// 4. A receiver that joins two DIFFERENT classes is `Object` with no
//    class, so the site keeps the plain helper call: an unproven site is
//    never given the inline form (docs/0010 decision 4). `on` sits at slot
//    1 of a Toggle and slot 0 of a Flag, so a site that reused the other
//    shape's slot would read the label string instead of the boolean.
function Toggle(on) {
  this.label = "toggle";
  this.on = on;
}
const t = new Toggle(false);
function pick(o) {
  return o.on;
}
console.log(pick(withExtra));
console.log(pick(t));
console.log(pick(withExtra));
console.log(t.label);

// 5. Out-of-line slots. ObjectHeader holds four inline slots and spills
//    the rest into an overflow block, which the inline fast path does not
//    cover — slot 5 has to fall through to the helper and still be right.
function Wide() {
  this.a = 1;
  this.b = 2;
  this.c = 3;
  this.d = 4;
  this.e = 5;
  this.f = 6;
}
const w = new Wide();
let acc = 0;
let j = 0;
while (j < 100) {
  acc = acc + w.a + w.f;
  j = j + 1;
}
console.log(acc);
console.log(w.e);
console.log(w.d);
