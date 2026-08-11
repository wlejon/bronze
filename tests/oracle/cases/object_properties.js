// Object literals and property reads: a literal builds a shape, and a
// property name reads the slot that shape assigned it (docs/0004).

const obj = { a: 10, b: 20 };
console.log(obj.a + obj.b);
