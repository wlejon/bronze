// Function.prototype.bind (ECMA-262 20.2.3.2) and the bound function it makes
// (10.4.1): call, construct, name, length, and bind-of-bind.
//
// Derived from ECMA-262:
//
// 1. 10.4.1.1: [[Call]] prepends the bound arguments and hands the target
//    [[BoundThis]] — so `add.bind(null, 1)(2, 3)` is `add(1, 2, 3)`, and not
//    even `.call` can re-aim a bound receiver.
// 2. 20.2.3.2 steps 2-3: `length` is the target's minus the bound count,
//    floored at zero; step 4: `name` is "bound " + the target's name.
//    Binding a bound function repeats both — "bound bound add", and the
//    length shrinks again.
// 3. 10.4.1.2: [[Construct]] goes to the TARGET with the bound arguments
//    prepended and [[BoundThis]] IGNORED, so `new P1(6)` builds a Point from
//    `Point.prototype` — `instanceof Point` holds — with x from the binding
//    and y from the call. A nested binding flattens through the chain the
//    same way.
function add(a, b, c) { return a + b + c; }
const add1 = add.bind(null, 1);
console.log(add1(2, 3));
console.log(typeof add1);
console.log(add1.name);
console.log(add1.length);
const add12 = add1.bind(undefined, 2);
console.log(add12(3));
console.log(add12.name);
console.log(add12.length);
function readTag() { return this.tag; }
const bound = readTag.bind({ tag: 'A' });
console.log(bound());
console.log(bound.call({ tag: 'B' }));
function Point(x, y) { this.x = x; this.y = y; }
const P1 = Point.bind(null, 5);
const p = new P1(6);
console.log(p.x, p.y);
console.log(p instanceof Point);
const P12 = P1.bind(null);
const q = new P12(7);
console.log(q.x, q.y);
console.log(add.bind(null, 1, 2, 3, 4).length);
