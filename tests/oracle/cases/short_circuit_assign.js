// Conditional assignment: the a || (a = ...) lazy-init family, plus
// `let x;` binding undefined at the declaration.
let noinit;
console.log(noinit);

let cache;
cache || (cache = 42);
console.log(cache);
cache || (cache = 99);
console.log(cache);

let b;
let r = b || (b = 7);
console.log(r);
console.log(b);

let c = 1;
let d = c && (c = 5);
console.log(c);
console.log(d);

let e;
let f = e ?? (e = 9);
console.log(e);
console.log(f);

let g = 0;
let h = 1 < 2 ? (g = 10) : (g = 20);
console.log(g);
console.log(h);

let k = 3;
let m = 0 ? (k = 99) : k + 1;
console.log(k);
console.log(m);

let q = 1;
0 || (q = "hello");
console.log(q);

let a2 = 0;
let b2 = 1;
console.log(a2 || (b2 && (a2 = 5)));
console.log(a2);

let t = 0;
let u = 1 < 2 ? (t = "s") : (t = 3);
console.log(t);
console.log(u);

let v = 0;
let w = (v = 2) ? (v = v + 1) : 0;
console.log(v);
console.log(w);

let x = 0;
let y = 0;
let i = 0;
while (i < 3) {
  (x = x + 1) && (y = y + 10);
  i = i + 1;
}
console.log(x);
console.log(y);
