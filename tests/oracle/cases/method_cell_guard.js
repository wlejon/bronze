// An inlined method body's guard asks a cell pair (shape, prototype epoch)
// before the site's method cache (runtime/accessor_cell.cpp,
// `bronze_method_cell_publish`). Each block warms a site on one class and
// then gives it what a stale pair would answer wrongly: a second shape, a
// subclass override, an own property of the method's name, the method
// replaced or deleted on the prototype, a prototype swap, a receiver that is
// no plain object.

class V {
  constructor(x) { this.x = x; }
  len() { return this.x * 2; }
  add(o) { this.x += o.x; return this; }
}
class W extends V {
  constructor(x) { super(x); this.w = 1; }
}
class O extends V {
  len() { return -this.x; }
}

function callLen(o) { return o.len() + 1; }
function chain(a, b) { return a.add(b).add(b).len(); }

let acc = 0;
for (let i = 0; i < 300; i++) acc += callLen(new V(i)) + chain(new V(i), new V(1));
console.log("warm " + acc);

// 1. a second shape resolving to the same method, alternating with the first.
acc = 0;
for (let i = 0; i < 300; i++) acc += callLen(i & 1 ? new W(i) : new V(i)) + chain(new W(i), new V(2));
console.log("1 " + acc);

// 2. a subclass override.
console.log("2 " + callLen(new O(5)) + " " + callLen(new V(5)) + " " + chain(new O(5), new V(1)));

// 3. an own property of the method's name, on a warmed shape's sibling.
const own = new V(4);
own.len = function () { return "own" + this.x; };
console.log("3 " + callLen(own) + " " + callLen(new V(4)));

// 4. the receiver gains the own method between two calls of one chain.
const g = new V(1);
const tricky = { get x() { g.add = function () { return { len() { return "swapped"; } }; }; return 1; } };
console.log("4 " + chain(g, tricky));

// 5. the method replaced on the prototype, then restored.
const orig = V.prototype.len;
V.prototype.len = function () { return 1000 + this.x; };
console.log("5a " + callLen(new V(1)) + " " + callLen(new W(1)));
V.prototype.len = orig;
console.log("5b " + callLen(new V(1)) + " " + callLen(new W(1)));

// 6. shadowed on a subclass prototype after the pair held that shape.
for (let i = 0; i < 50; i++) callLen(new W(i));
W.prototype.len = function () { return "W" + this.x; };
console.log("6 " + callLen(new W(3)) + " " + callLen(new V(3)));
delete W.prototype.len;
console.log("6b " + callLen(new W(3)));

// 7. a prototype swap on an instance.
const s = new V(9);
Object.setPrototypeOf(s, { len() { return "other"; } });
console.log("7 " + callLen(s));

// 8. receivers that are not plain objects, or have no such method.
const fn = function () {};
fn.len = function () { return 41; };
console.log("8 " + callLen(fn) + " " + callLen({ len: () => 6 }) + " " + callLen("abc".split("")
  .map((c) => ({ len() { return c.length; } }))[0]));
try { callLen({}); } catch (e) { console.log("8 " + (e instanceof TypeError)); }
try { callLen(null); } catch (e) { console.log("8 " + (e instanceof TypeError)); }

// 9. deleted from the prototype: the call throws, then works again.
delete V.prototype.len;
try { callLen(new V(1)); } catch (e) { console.log("9 " + (e instanceof TypeError)); }
V.prototype.len = orig;
console.log("9 " + callLen(new V(1)));
