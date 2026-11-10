// Native builtins entered at the method site (il_native_fast.h): Array
// indexOf / includes / push / pop and String charCodeAt / charAt / indexOf /
// startsWith / slice / substring, each in a loop (so the site is compiled for
// more than one run) with the receivers and arguments the guards accept and
// every kind they must hand back to the site's ordinary call.

function show(label, v) {
  console.log(label, typeof v === "string" ? JSON.stringify(v) : String(v));
}

// ---- Array -----------------------------------------------------------------

function arrSearch(a, x) { return a.indexOf(x) + ":" + a.includes(x); }

const plain = [1, 2, NaN, -0, "s", undefined, null, 3n, true];
const needles = [1, 2, NaN, 0, -0, "s", "S", undefined, null, 3n, true, false, {}, 7];
for (let r = 0; r < 3; ++r) {
  for (const n of needles) show("plain " + String(n), arrSearch(plain, n));
}

// Holes read as undefined for includes, absent for indexOf; a chain with an
// index reads through them (Array.prototype refuses an index in bronze, so
// the index is on Object.prototype).
const holey = [1, , 3];
for (let r = 0; r < 3; ++r) show("holey", arrSearch(holey, undefined));
Object.prototype[1] = "chain";
for (let r = 0; r < 2; ++r) show("chain", arrSearch(holey, "chain") + " " + arrSearch(holey, undefined));
delete Object.prototype[1];
for (let r = 0; r < 2; ++r) show("unchained", arrSearch(holey, "chain"));

// Receivers the Array guard refuses.
class Sub extends Array {}
const sub = Sub.from([5, 6, 7]);
const boxed = [5, 6, 7];
boxed.extra = 1;
const own = [5, 6, 7];
own.indexOf = function (x) { return "own " + x; };
own.includes = function (x) { return "own includes " + x; };
const proxy = new Proxy([5, 6, 7], {});
const arrayLike = { length: 3, 0: 5, 1: 6, 2: 7, indexOf: Array.prototype.indexOf, includes: Array.prototype.includes };
const frozen = Object.freeze([5, 6, 7]);
for (let r = 0; r < 3; ++r) {
  for (const a of [sub, boxed, own, proxy, arrayLike, frozen, [5, 6, 7]]) show("recv", arrSearch(a, 6));
}

// .call / .apply / bind with other receivers.
const io = Array.prototype.indexOf;
for (let r = 0; r < 2; ++r) {
  show("call str", io.call("abc", "b"));
  show("apply", io.apply([1, 2, 3], [3]));
  show("bind", io.bind([9, 8])(8));
  try { io.call(null, 1); } catch (e) { show("call null", e.constructor.name); }
  try { io.call(undefined, 1); } catch (e) { show("call undefined", e.constructor.name); }
  show("call num", io.call(5, 1));
}

// Strings and BigInts compare by content.
const strs = ["a" + "b", "c", 10n ** 20n];
for (let r = 0; r < 2; ++r) show("content", arrSearch(strs, "ab") + " " + arrSearch(strs, 100000000000000000000n));

// push / pop, any argument count, past capacity, on refused receivers too.
function pushPop(a, n) {
  let s = 0;
  for (let i = 0; i < n; ++i) s += a.push(i, i + 1);
  for (let i = 0; i < n; ++i) s += a.push(i);
  for (let i = 0; i < n; ++i) s += a.push(i, i, i, i);
  let t = 0;
  for (let i = 0; i < n; ++i) { const v = a.pop(); t += typeof v === "number" ? v : 1000; }
  return s + "/" + t + "/" + a.length;
}
for (let r = 0; r < 3; ++r) show("pushpop", pushPop([], 50));
for (let r = 0; r < 2; ++r) show("pushpop sub", pushPop(new Sub(), 10));
for (let r = 0; r < 2; ++r) show("pushpop boxed", pushPop(Object.assign([], { x: 1 }), 10));
const objs = [];
for (let r = 0; r < 3; ++r) for (let i = 0; i < 100; ++i) objs.push({ i: i, r: r }, [i]);
let objSum = 0;
while (objs.length) { const o = objs.pop(); objSum += Array.isArray(o) ? o[0] : o.i; }
show("objs", objSum);
try { Object.freeze([1]).push(2); } catch (e) { show("frozen push", e.constructor.name); }
try { Object.freeze([1]).pop(); } catch (e) { show("frozen pop", e.constructor.name); }
const setterHole = [];
Object.defineProperty(Object.prototype, 3, { set(v) { show("proto setter", v); }, configurable: true });
for (let i = 0; i < 6; ++i) setterHole.push(i);
show("setterHole", setterHole.length + " " + setterHole.join());
delete Object.prototype[3];

// ---- String ----------------------------------------------------------------

function readers(s, i) {
  return s.charCodeAt(i) + "|" + s.charAt(i) + "|" + s.charCodeAt() + "|" + s.charAt();
}
const positions = [0, 1, 2.7, -1, -0, 5, 100, NaN, Infinity, -Infinity, undefined, null, "1", true, 2 ** 32];
for (let r = 0; r < 3; ++r) {
  for (const p of positions) show("read " + String(p), readers("héllo\u{1F600}", p));
}
const valueOfPos = { valueOf() { show("valueOf ran", 1); return 1; } };
for (let r = 0; r < 2; ++r) show("valueOf pos", readers("abc", valueOfPos));
for (let r = 0; r < 2; ++r) {
  try { "abc".charCodeAt(Symbol()); } catch (e) { show("symbol pos", e.constructor.name); }
  try { "abc".charAt({ valueOf() { throw new RangeError("pos"); } }); } catch (e) { show("throwing pos", e.message); }
}

function searches(s, t, p) {
  return s.indexOf(t) + "|" + s.indexOf(t, p) + "|" + s.startsWith(t) + "|" + s.startsWith(t, p);
}
const hay = "abcabcĀabc";
const searchArgs = [["abc", 1], ["", 3], ["Ā", 0], ["c", -5], ["c", 100], ["x", 0], [1, 0],
                    [undefined, 0], [null, 0], ["b", undefined], ["b", "2"], ["b", NaN]];
for (let r = 0; r < 3; ++r) {
  for (const [t, p] of searchArgs) show("search " + String(t) + "," + String(p), searches(hay, t, p));
  show("latin hay", searches("undefined null", undefined, 0) + " " + searches("abc", "Ā", 0));
}
for (let r = 0; r < 2; ++r) {
  show("toString search", "a1b".indexOf({ toString() { return "1b"; } }));
}

function slices(s, a, b) {
  return [s.slice(a, b), s.slice(a), s.slice(), s.substring(a, b), s.substring(a), s.substring()].join("|");
}
const sliceArgs = [[1, 3], [3, 1], [-2, undefined], [-2, -1], [0, 100], [NaN, 2], [2, NaN], [1.9, 3.1],
                   [-Infinity, Infinity], [undefined, 2], ["1", "3"], [null, 2], [true, 3]];
for (let r = 0; r < 3; ++r) {
  for (const [a, b] of sliceArgs) show("slice " + String(a) + "," + String(b), slices("hello☃world", a, b));
}

// Receivers the String guard refuses or that change under it.
function charLoop(s) { let n = 0; for (let i = 0; i < 4; ++i) n += s.charCodeAt(i) | 0; return n; }
const wrapper = new String("abcd");
for (let r = 0; r < 3; ++r) show("wrapper", charLoop(wrapper) + " " + charLoop("abcd"));
try { String.prototype.charCodeAt.call(null, 0); } catch (e) { show("null this", e.constructor.name); }
try { String.prototype.slice.call(undefined); } catch (e) { show("undefined this", e.constructor.name); }
show("num this", String.prototype.charAt.call(12345, 2) + String.prototype.slice.call(12345, 1, 3));
show("obj this", String.prototype.indexOf.call({ toString() { return "xyz"; } }, "z"));
show("bound", String.prototype.substring.bind("bound!")(1, 4));
show("apply", String.prototype.startsWith.apply("apply", ["ap"]));

// The member replaced in place (the shape does not move), then deleted, then
// restored, while the same sites keep running.
walk();  // before String.prototype is patched (and, once deleted from, a dictionary)
const originalCharCodeAt = String.prototype.charCodeAt;
const originalSlice = String.prototype.slice;
for (let r = 0; r < 3; ++r) show("before", charLoop("abcd") + " " + slices("abcdef", 1, 3));
String.prototype.charCodeAt = function (i) { return 1000 + (i | 0); };
String.prototype.slice = function (a, b) { return "patched(" + a + "," + b + ")"; };
for (let r = 0; r < 3; ++r) show("replaced", charLoop("abcd") + " " + slices("abcdef", 1, 3));
delete String.prototype.charCodeAt;
for (let r = 0; r < 2; ++r) {
  try { charLoop("abcd"); } catch (e) { show("deleted", e.constructor.name); }
}
String.prototype.charCodeAt = originalCharCodeAt;
String.prototype.slice = originalSlice;
for (let r = 0; r < 3; ++r) show("restored", charLoop("abcd") + " " + slices("abcdef", 1, 3));
Object.prototype.charAt = function () { return "object proto"; };
for (let r = 0; r < 2; ++r) show("object proto", readers("abc", 1));
delete Object.prototype.charAt;

// A polymorphic site: strings and arrays through one `indexOf`.
function poly(x, y) { return x.indexOf(y); }
for (let r = 0; r < 3; ++r) {
  show("poly", [poly("abc", "c"), poly([1, 2, 3], 3), poly("abc", "z"), poly([1], "1"), poly({ indexOf: () => "o" }, 1)].join());
}

// Re-entrancy: a conversion that runs the same sites.
const reenter = { valueOf() { return "xyz".charCodeAt(0) - 119 + [7, 8].indexOf(8); } };
for (let r = 0; r < 2; ++r) show("reenter", "abcdefg".charAt(reenter) + "abcdefg".slice(reenter, 5));

// A throw out of a conversion, caught, with the stack naming the caller.
function thrower() {
  return "abc".slice({ valueOf() { throw new Error("from valueOf"); } });
}
for (let r = 0; r < 2; ++r) {
  try { thrower(); } catch (e) {
    show("caught", e.message + " " + /thrower/.test(String(e.stack)));
  }
}

// A longer walk: GC pressure from the allocating members.
function walk() {
let acc = 0;
let pieces = [];
const text = "The quick brown fox jumps over the lazy dog éè 中文 ";
for (let i = 0; i < 20000; ++i) {
  const j = i % text.length;
  acc = (acc * 31 + text.charCodeAt(j)) | 0;
  const c = text.charAt(j);
  pieces.push(text.slice(j, j + 3) + c + text.substring(j + 2, j));
  if (text.startsWith("quick", j)) acc ^= j;
  acc += text.indexOf(c, j) + pieces.indexOf(c);
  if (pieces.length > 500) pieces = [];
}
show("walk", acc + " " + pieces.length + " " + pieces[pieces.length - 1]);
}
walk();
