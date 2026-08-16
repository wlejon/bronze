// ECMA-262 13.15.2: `x op= y` is `x op y` written once. That identity is the
// whole subject of this case — every compound operator has to reach the same
// algorithm its plain binary form reaches, over every reference kind the
// language has (a binding, `o.k`, `o[k]`), and a BigInt is where the two used
// to come apart: `-=` `*=` `/=` `%=` unboxed to a double unconditionally on
// the belief that they are ToNumber, where 13.15.3 makes them ToNumERIC.
function fail(f) {
  try { return String(f()); } catch (e) {
    const kind = e instanceof RangeError ? "RangeError" : e instanceof TypeError ? "TypeError" : "Error";
    return kind + ": " + e.message;
  }
}

// ---- a local binding --------------------------------------------------------
let a = 10n; a += 2n; console.log(a, typeof a);
let b = 10n; b -= 2n; console.log(b, typeof b);
let c = 10n; c *= 3n; console.log(c, typeof c);
let d = 17n; d /= 5n; console.log(d, typeof d);
let e = 17n; e %= 5n; console.log(e, typeof e);
let f = 2n; f **= 64n; console.log(f, typeof f);
let g = 12n; g &= 10n; console.log(g, typeof g);
let h = 12n; h |= 3n; console.log(h, typeof h);
let i = 12n; i ^= 10n; console.log(i, typeof i);
let j = 1n; j <<= 64n; console.log(j, typeof j);
let k = -1n; k >>= 100n; console.log(k, typeof k);

// The whole family again, chained, so each result feeds the next.
let z = 1n;
z += 9n; z *= 9007199254740993n; z -= 1n; z /= 2n; z %= 1000000007n; z **= 2n;
console.log(z);

// ---- an object property -----------------------------------------------------
const o = { v: 10n };
o.v += 2n; console.log(o.v);
o.v -= 4n; console.log(o.v);
o.v *= 5n; console.log(o.v);
o.v /= 3n; console.log(o.v);
o.v %= 7n; console.log(o.v);
o.v **= 3n; console.log(o.v);
o.v &= 6n; console.log(o.v);
o.v |= 9n; console.log(o.v);
o.v ^= 3n; console.log(o.v);
o.v <<= 40n; console.log(o.v);
o.v >>= 2n; console.log(o.v);

// ---- a computed index -------------------------------------------------------
const arr = [100n, -100n];
arr[0] += 1n; console.log(arr[0]);
arr[0] -= 3n; console.log(arr[0]);
arr[0] *= 2n; console.log(arr[0]);
arr[0] /= 4n; console.log(arr[0]);
arr[0] %= 20n; console.log(arr[0]);
arr[1] **= 3n; console.log(arr[1]);
arr[1] >>= 8n; console.log(arr[1]);
arr[1] &= -1n; console.log(arr[1]);
const key = 0;
arr[key] <<= 32n; console.log(arr[key]);

// ---- the update operators on the same three reference kinds -----------------
let u = 9007199254740993n; u++; console.log(u); u--; u--; console.log(u);
const uo = { v: 2n ** 64n }; uo.v++; console.log(uo.v); uo.v--; uo.v--; console.log(uo.v);
const ua = [2n ** 64n]; ua[0]++; console.log(ua[0]); ua[0]--; ua[0]--; console.log(ua[0]);
let post = 5n; console.log(post++, post, post--, post);
let pre = 5n; console.log(++pre, pre, --pre, pre);

// ---- mixing, which is a TypeError through the compound spelling too ---------
console.log(fail(() => { let m = 1n; m -= 1; return m; }));
console.log(fail(() => { let m = 1n; m *= 2; return m; }));
console.log(fail(() => { let m = 1n; m /= 1; return m; }));
console.log(fail(() => { let m = 1n; m %= 1; return m; }));
console.log(fail(() => { let m = 1n; m += 1; return m; }));
console.log(fail(() => { let m = 1n; m **= 2; return m; }));
console.log(fail(() => { let m = 1n; m <<= 1; return m; }));
console.log(fail(() => { const mo = { v: 1n }; mo.v -= 1; return mo.v; }));
console.log(fail(() => { const ma = [1n]; ma[0] -= 1; return ma[0]; }));
console.log(fail(() => { let m = 1n; m /= 0n; return m; }));
// `+=` with a STRING is not mixing at all: 13.15.3 step 1.d concatenates.
let s = "n="; s += 1n; console.log(s, typeof s);
let t = 1n; t += ""; console.log(t, typeof t);

// ---- the number-typed path, which must be exactly what it always was --------
let n = 10; n += 2; n -= 4; n *= 5; n /= 4; n %= 7; n **= 2; console.log(n, typeof n);
let p = 12; p &= 10; p |= 3; p ^= 6; p <<= 3; p >>= 1; p >>>= 1; console.log(p, typeof p);
let q = 5; q++; q--; q--; console.log(q, typeof q);
const no = { v: 10 }; no.v += 2; no.v *= 3; no.v -= 6; console.log(no.v, typeof no.v);
const na = [10]; na[0] /= 4; na[0] %= 2; na[0] += 0.5; console.log(na[0], typeof na[0]);
let str = "a"; str += 1; str += true; console.log(str, typeof str);
