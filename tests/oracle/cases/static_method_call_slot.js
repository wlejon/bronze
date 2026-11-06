// Calls of a class's static methods (`Q.slerpFlat(...)` in three.js), many
// times from one site so the site's cache settles on the statics' slot, then
// with the static replaced, deleted, shadowed by an accessor, made a
// non-function, and with receivers of other kinds at the same site.
class Q {
    static lerp(dst, a, b, t) {
        dst[0] = a[0] + (b[0] - a[0]) * t;
        return this === Q ? 'Q' : typeof this;
    }
    static count() { return arguments.length; }
    static pad(a, b, c) { return [a, b, c].map(String).join('/'); }
}
class R extends Q {}

function run(C, n) {
    const dst = new Float32Array(1), a = new Float64Array([1]), b = new Float64Array([3]);
    let s = 0, who = '';
    for (let i = 0; i < n; i++) {
        who = C.lerp(dst, a, b, i / n);
        s += dst[0];
    }
    return s.toFixed(3) + ' ' + who;
}
console.log(run(Q, 2000));
console.log(run(R, 50));
console.log(run(Q, 10));

function args(C) { let t = 0; for (let i = 0; i < 300; i++) t += C.count(1, 2, i); return t; }
console.log(args(Q));
function pads(C) { let r = ''; for (let i = 0; i < 200; i++) r = C.pad(i); return r; }
console.log(pads(Q));

// replaced: the site must call the new function
const old = Q.lerp;
Q.lerp = function (dst) { dst[0] = 42; return 'replaced'; };
console.log(run(Q, 5));
Q.lerp = old;
console.log(run(Q, 5));

// a plain object with the same method name at the same site
const plain = { lerp(dst) { dst[0] = 1; return 'plain'; } };
console.log(run(plain, 5));
console.log(run(Q, 5));

// a getter in place of the data property
Object.defineProperty(Q, 'lerp', { get() { return function (dst) { dst[0] = 2; return 'getter'; }; }, configurable: true });
console.log(run(Q, 5));
Object.defineProperty(Q, 'lerp', { value: old, writable: true, configurable: true });
console.log(run(Q, 5));

// a non-function: TypeError
Q.lerp = 7;
try { run(Q, 1); } catch (e) { console.log(e instanceof TypeError); }
delete Q.lerp;
try { run(Q, 1); } catch (e) { console.log(e instanceof TypeError); }
Q.lerp = old;
console.log(run(Q, 3));

// a bound function and an arrow stored as a static
Q.lerp = old.bind(R);
console.log(run(Q, 3));
Q.lerp = (dst) => { dst[0] = 9; return 'arrow'; };
console.log(run(Q, 3));

// another class's statics at the site, one after the other
class S { static lerp(dst) { dst[0] = 5; return 'S'; } }
for (let k = 0; k < 3; k++) console.log(run(S, 4), run(Q, 4));
