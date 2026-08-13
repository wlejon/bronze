// WeakSet (ECMA-262 24.4): add/has/delete, the CanBeHeldWeakly split, the
// iterable constructor argument, and the absences — no size, no iteration —
// that 24.4 shares with 24.3.
//
// Derived from ECMA-262:
//
// 1. 24.4.3.1 `add` returns the set itself.
// 2. 24.4.3.3 `delete` answers whether something was removed; asking twice is
//    true then false.
// 3. Reads are quiet about a primitive (24.4.3.4 step 4 returns false before
//    the table is consulted); the WRITE is the TypeError 24.4.3.1 step 4
//    names.
// 4. 24.4.1.1 walks an iterable of values through the same adder.
// 5. 24.4.3.5's @@toStringTag is "WeakSet".
// 6. `get` belongs to 24.3 and not 24.4: `'get' in ws` is false, because the
//    two prototypes are different member lists and not one list with two
//    names.
const ws = new WeakSet();
const a = {};
const b = {};
console.log(ws.add(a) === ws);
console.log(ws.has(a), ws.has(b));
console.log(ws.delete(a), ws.delete(a));
console.log(ws.has(a));
console.log(ws.has('x'), ws.delete(5));
try {
  ws.add(3);
} catch (e) {
  console.log(e instanceof TypeError);
}
const ws2 = new WeakSet([a, b]);
console.log(ws2.has(a), ws2.has(b));
console.log(typeof ws);
console.log(Object.prototype.toString.call(ws));
console.log(ws[Symbol.iterator]);
console.log('add' in ws, 'get' in ws);
