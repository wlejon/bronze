// A profiled build interprets the functions its harvest saw run seldom or
// never (docs/type-feedback.md "Cold functions"): compiled and interpreted
// frames then share one stack. Throws cross the boundary both ways, a stack
// trace names both kinds of frame, recursion alternates between them, and
// closures made on either side are called from the other.

function frames(stack) {
  const m = stack.match(/at (\S+)/g);
  return m ? m.map(s => s.replace('at ', '')) : [];
}

// Hot: called in the warm-up loop below.
function hotAdd(a, b) { return a + b; }
function hotThrowIf(x) {
  if (x === 7) throw new RangeError('hot ' + x);
  return x * 2;
}
function hotCatches(f, x) {
  try {
    return f(x);
  } catch (e) {
    return 'caught ' + e.message;
  }
}
function hotDown(n) { return n <= 0 ? 0 : 1 + coldDown(n - 1); }
function hotApply(f, x) { return f(x); }
function hotMakeAdder(k) { return v => v + k; }

let warm = 0;
for (let i = 0; i < 20000; i++) {
  warm = hotAdd(warm, i & 3);
  warm += hotThrowIf(i & 3);
  warm += hotCatches(hotThrowIf, i & 3);
  warm += hotDown(i & 1);
  warm += hotApply(hotMakeAdder(1), i & 1);
}
console.log('warm', warm);

// Cold: each called a few times at most.
function coldThrows(x) { throw new TypeError('cold ' + x); }
function coldCatchesHot(x) {
  try {
    return hotThrowIf(x);
  } catch (e) {
    return e.name + ':' + e.message;
  }
}
function coldDown(n) { return n <= 0 ? 0 : 1 + hotDown(n - 1); }
function coldStack() { return frames(new Error('s').stack); }
function coldViaHot() { return hotApply(() => coldStack(), 0); }
function coldMakeCounter() {
  let n = 0;
  return () => ++n;
}
function coldCallsHotClosure(f) {
  let s = 0;
  for (let i = 0; i < 5; i++) s += f(i);
  return s;
}

console.log(hotCatches(coldThrows, 3));
console.log(coldCatchesHot(7), coldCatchesHot(4));
console.log(coldDown(9), hotDown(10));
// The named frames (an arrow function's has no name to compare).
const st = coldViaHot().filter(n => /^[A-Za-z_$][\w$.]*$/.test(n));
console.log(st[0], st[1], st[2]);
const counter = coldMakeCounter();
let c = 0;
for (let i = 0; i < 3000; i++) c = hotApply(counter, 0);
console.log('counter', c);
console.log('adder', coldCallsHotClosure(hotMakeAdder(10)));

// A throw out of an interpreted frame through compiled ones to an
// interpreted catch, and the reverse.
function coldOuter() {
  try {
    return hotApply(coldThrows, 'deep');
  } catch (e) {
    return 'outer ' + e.message;
  } finally {
    console.log('finally cold');
  }
}
console.log(coldOuter());
function hotOuter(f) {
  try {
    return hotApply(f, 1);
  } catch (e) {
    return 'hot outer ' + e.message;
  }
}
for (let i = 0; i < 2000; i++) hotOuter(x => x);
console.log(hotOuter(coldThrows));
