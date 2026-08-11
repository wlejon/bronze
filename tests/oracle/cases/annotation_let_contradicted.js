// An annotation on a declaration is a hint about the binding, not a cast of
// the initialiser (docs/0010 decision 6). `let tag: number = "seven"` used to
// emit an unbox of a boxed string; the binding is a string, and stays one.
let tag: number = "seven";
console.log(tag);
console.log(tag + 1);

// The same annotation where the initialiser agrees: proven, silent, and the
// binding holds an unboxed double.
let n: number = 7;
console.log(n + 1);

// `let u: number;` binds undefined at the declaration — the TDZ ends there,
// not at the first assignment (ECMA-262 14.3.1.2). An annotation does not buy
// the binding a typed slot that undefined cannot fit into, so this prints
// undefined rather than failing to compile.
let u: number;
console.log(u);
u = "later";
console.log(u);
