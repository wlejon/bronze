// A METHOD CALL whose RECEIVER IS A FUNCTION — `Object.defineProperty(...)`,
// `A.make()`, `Promise.resolve(...)` — answered out of the function's statics
// box through the method site's inline cache (llvm_method_call.cpp's statics
// arm, rt_method_call.cpp's `latchFunctionStaticsMethodIc`).
//
// The call twin of `fn_statics_ic.js`, which pins the READ. Everything that
// file says about the substitution still holds — a function's `static` members
// live in a side object hanging off the FunctionHeader, so the cached shape and
// slot describe a box that is not the thing the source names — and a call adds
// two questions a read never asks:
//
//   - WHO is `this`? The FUNCTION, always. The box is where the method was
//     found, which is not who it was called on.
//   - WHAT is being called? The entry caches only WHERE, so every hit
//     re-derives code, environment and arity from the value in that slot
//     RIGHT NOW and re-checks that it is a function at all. Replacing,
//     deleting, or overwriting a static with a non-function must therefore
//     land exactly where it did before the site was ever warm.
//
// Every case is warmed FIRST and disturbed AFTERWARDS, because a cold site
// proves nothing about a cache.
//
// Clause references are ECMA-262 (2024).

function say(label, value) { console.log(label + '=' + value); }

// --- 1. The warm static call, and the three ways the slot can change --------
class A {
  static hello(x) { return 'hi:' + x; }
}
let r = '';
for (let i = 0; i < 6; i++) r = A.hello(i);
say('warm.hello', r);                        // last iteration is i=5: 'hi:5'

// REPLACED. An ordinary set on an existing own property of the box: same
// shape, same slot, a DIFFERENT function. The entry still describes the box,
// so it still hits — and a hit that had cached a code pointer would call the
// function that is no longer there.
A.hello = function (x) { return 'replaced:' + x; };
let r2 = '';
for (let i = 0; i < 6; i++) r2 = A.hello(i);
say('replaced.hello', r2);                   // 'replaced:5'

// NON-FUNCTION. 13.3.6.1 evaluates the member and then requires it callable;
// the slot now holds a Number, so the inline arm's own function test sends the
// call to the helper and its TypeError.
A.hello = 42;
let r3 = '';
try { r3 = A.hello(1); } catch (e) { r3 = e.name; }
say('nonfn.call', r3);                       // 'TypeError'

// RESTORED. The same site, callable again, with no relatch needed: the shape
// never moved, only the slot's contents.
A.hello = function (x) { return 'restored:' + x; };
let r4 = '';
for (let i = 0; i < 4; i++) r4 = A.hello(i);
say('restored.hello', r4);                   // 'restored:3'

// DELETED. 13.5.1.2 removes the own property and the box goes to dictionary
// mode, which every fill path refuses; the member reads `undefined` and the
// call is 13.3.6.1's TypeError.
class DEL { static go() { return 'go'; } }
let dwarm = '';
for (let i = 0; i < 4; i++) dwarm = DEL.go();
say('deleted.before', dwarm);                // 'go'
delete DEL.go;
let dg = '';
try { dg = DEL.go(); } catch (e) { dg = e.name; }
say('deleted.call', dg);                     // 'TypeError'

// --- 2. A static REDEFINED AS AN ACCESSOR that returns a function -----------
// The receiver a `static get` sees is the CLASS (15.7.14), not the box the
// getter is stored in, so an accessor must never be spent by the inline arm —
// and the function the getter RETURNS is then what gets called.
class B { static v() { return 'data-v'; } }
let bv = '';
for (let i = 0; i < 4; i++) bv = B.v();
say('accessor.before', bv);                  // 'data-v'
Object.defineProperty(B, 'v', {
  get() { return function () { return 'from-getter'; }; }, configurable: true
});
let bv2 = '';
for (let i = 0; i < 4; i++) bv2 = B.v();
say('accessor.after', bv2);                  // 'from-getter'

// --- 3. INHERITED statics: `class Q extends P` ------------------------------
// 15.7.14 sets Q's [[Prototype]] to P, and bronze links Q's statics box above
// P's. `Q.make` is NOT an own property of Q's box, so the arm must refuse it
// and the helper must still answer — with `this` bound to Q.
class P {
  static label = 'P';
  static make() { return 'make:' + this.label; }
}
class Q extends P { static label = 'Q'; }
let pm = '';
for (let i = 0; i < 5; i++) pm = P.make();
say('own.make', pm);                         // 'make:P'
let qm = '';
for (let i = 0; i < 5; i++) qm = Q.make();
say('inherited.make', qm);                   // 'make:Q' — receiver is Q, method is P's

// SHADOWED on the subclass afterwards: now it IS an own property of R's box.
class R extends P { static label = 'R'; }
let rm = '';
for (let i = 0; i < 5; i++) rm = R.make();
say('inherited.shadowable', rm);             // 'make:R', still through P's method
R.make = function () { return 'own-make:' + this.label; };
let rm2 = '';
for (let i = 0; i < 5; i++) rm2 = R.make();
say('shadowed.make', rm2);                   // 'own-make:R'

// --- 4. `this` IS THE FUNCTION, never the box ------------------------------
// The one thing this arm could get wrong that no read ever could.
class T {
  static tag = 'T';
  static who() { return this === T; }
  static whoTag() { return this.tag; }
}
let w = false;
for (let i = 0; i < 5; i++) w = T.who();
say('this.identity', w);                     // true
let wt = '';
for (let i = 0; i < 5; i++) wt = T.whoTag();
say('this.tag', wt);                         // 'T'

class U extends T { static tag = 'U'; }
let uw = true;
for (let i = 0; i < 5; i++) uw = U.who();
say('this.subclass.identity', uw);           // false — `this` is U
let ut = '';
for (let i = 0; i < 5; i++) ut = U.whoTag();
say('this.subclass.tag', ut);                // 'U' — U's own field, T's method

// --- 5. A BOUND static ------------------------------------------------------
// 10.4.1's exotic object ignores the call's receiver entirely, so a warm site
// that substituted the box for `this` would be invisible here and visible
// everywhere else. Pinned so the two answers cannot drift apart.
class V { static tag = 'V'; static read() { return 'g:' + this.tag; } }
V.bound = V.read.bind({ tag: 'BOUND' });
let bs = '';
for (let i = 0; i < 5; i++) bs = V.bound();
say('bound.static', bs);                     // 'g:BOUND'
let bu = '';
for (let i = 0; i < 5; i++) bu = V.read();
say('bound.unbound', bu);                    // 'g:V' — the same method, unbound

// --- 6. `Object.*` in a loop: the case the arm was built for ----------------
// three.js's `Object3D` constructor calls `Object.defineProperty` and
// `Object.defineProperties` on every instance it builds. %Object% is a
// function object whose members are own data properties of a real statics box
// (abi/bronze_global_statics.h), which is what makes them cacheable at all —
// %Array% and %Number% answer from a C table and are refused.
function Node3(id) {
  Object.defineProperty(this, 'id', { value: id, configurable: true });
  Object.defineProperties(this, {
    kind: { value: 'node', enumerable: true },
    seq: { value: id * 2, enumerable: true }
  });
}
let ids = 0;
let last = '';
for (let i = 0; i < 6; i++) {
  const n = new Node3(i);
  ids += n.id;
  last = n.kind + ':' + n.seq;
}
say('defineProperty.sum', ids);              // 0+1+2+3+4+5 = 15
say('defineProperties.last', last);          // i=5: 'node:10'
say('defineProperties.keys', Object.keys(new Node3(1)).join(','));
                                             // 'kind,seq' — `id` is not enumerable
const desc = Object.getOwnPropertyDescriptor(new Node3(9), 'id');
say('defineProperty.desc', desc.value + ',' + desc.enumerable + ',' + desc.configurable);
                                             // '9,false,true' (6.2.6.6 defaults)
let keys = '';
for (let i = 0; i < 6; i++) keys = Object.keys({ a: 1, b: 2, c: 3 }).join(',');
say('Object.keys', keys);                    // 'a,b,c'

// --- 7. A static that THROWS on the third call ------------------------------
// The exception leaves through the inline path, not the helper's, on the third
// iteration of a site that is warm by then.
class W {
  static n = 0;
  static step() {
    W.n = W.n + 1;
    if (W.n === 3) throw new Error('boom-3');
    return 'step-' + W.n;
  }
}
let tlog = '';
for (let i = 0; i < 5; i++) {
  try { tlog += W.step() + ' '; } catch (e) { tlog += '[' + e.message + '] '; }
}
say('throwing.static', tlog.trim());
                                  // 'step-1 step-2 [boom-3] step-4 step-5'

// --- 8. One site over a CLASS and a PLAIN OBJECT ---------------------------
// The polymorphism the arm is most exposed to. A statics box and a plain
// object are both plain objects from the same shape arena and can hold the
// very same shape pointer — `{tag}` at slot 0 for both of these. That is sound
// precisely because a shape is a key-to-slot map and nothing more: whichever
// receiver matched, the site's key is at that slot IN ITSELF. It would not be
// sound for a cached code pointer, which is why this entry caches only where.
function callTag(o) { return o.tag(); }
class X { static tag() { return 'X-static'; } }
const plainX = { tag: function () { return 'plain-own'; } };
let mixed = '';
for (let i = 0; i < 6; i++) mixed += callTag(X) + '/' + callTag(plainX) + ' ';
say('mixed.fn.plain', mixed.trim());         // six repeats of 'X-static/plain-own'

// Retiring the plain receiver's shape must leave the class's answer alone.
plainX.other = 1;
let post = '';
for (let i = 0; i < 4; i++) post = callTag(X) + '/' + callTag(plainX);
say('post.mutation', post);                  // 'X-static/plain-own'

// --- 9. WRONG ARITY through the cached triple ------------------------------
// The entry carries no arity of its own — it is read off the function in the
// slot at every hit — and the call site's argc is fixed at compile time. Under
// arity the inline path pads with `undefined` (10.2.11's [[Call]] over a short
// argument list); over arity the extras are simply not named.
class Y { static three(a, b, c) { return 'three:' + a + ',' + b + ',' + c; } }
let a0 = '', a1 = '', a3 = '', a5 = '';
for (let i = 0; i < 4; i++) a0 = Y.three();
for (let i = 0; i < 4; i++) a1 = Y.three(1);
for (let i = 0; i < 4; i++) a3 = Y.three(1, 2, 3);
for (let i = 0; i < 4; i++) a5 = Y.three(1, 2, 3, 4, 5);
say('arity.0', a0);                          // 'three:undefined,undefined,undefined'
say('arity.1', a1);                          // 'three:1,undefined,undefined'
say('arity.3', a3);                          // 'three:1,2,3'
say('arity.over', a5);                       // 'three:1,2,3'

// --- 10. `new this()` through a static -------------------------------------
// A static factory: the receiver decides the class that gets built, so a `this`
// the arm had substituted would construct the wrong thing — or the box.
class Z {
  constructor(v) { this.made = 'made-' + v; }
  static kind = 'Z';
  static create() { return new this(this.kind); }
}
let made = '';
for (let i = 0; i < 5; i++) made = Z.create().made;
say('static.new.this', made);                // 'made-Z'
class Z2 extends Z { static kind = 'Z2'; }
let made2 = '';
for (let i = 0; i < 5; i++) made2 = Z2.create().made;
say('static.new.subclass', made2);           // 'made-Z2' — `create` inherited, `this` is Z2

// --- 11. %Function.prototype% methods through a warm site ------------------
// `call`, `apply` and `bind` (20.2.3) are answered BELOW the box in the
// runtime's order, so they must miss the arm every time — and a class whose
// own static IS named `call` shadows the inherited one (15.7.14), through the
// same site.
function fnA(x) { return 'fnA:' + x; }
function callThrough(o) { return o.call(null, 7); }
class S { static call(v) { return 'own-call:' + v; } }
let c1 = '';
for (let i = 0; i < 4; i++) c1 = callThrough(fnA);
say('proto.call', c1);                       // 'fnA:7'
let c2 = '';
for (let i = 0; i < 4; i++) c2 = callThrough(S);
say('own.call', c2);                         // 'own-call:null' — the static, argument null
let c3 = '';
for (let i = 0; i < 4; i++) c3 = callThrough(fnA);
say('proto.call.again', c3);                 // 'fnA:7' — the site is not poisoned

function addTwo(a, b) { return 'sum:' + (a + b); }
function applyThrough(o) { return o.apply(null, [3, 4]); }
let ap = '';
for (let i = 0; i < 4; i++) ap = applyThrough(addTwo);
say('proto.apply', ap);                      // 'sum:7'
function bindThrough(o) { return o.bind(null, 10); }
let br = '';
for (let i = 0; i < 4; i++) br = bindThrough(addTwo)(5);
say('proto.bind', br);                       // addTwo(10, 5): 'sum:15'

// --- 12. An ORDINARY FUNCTION carrying a static ----------------------------
// Nothing about the mechanism is about classes; a function with a property
// hung on it has the same box.
function fnHost() { return 'host'; }
fnHost.helper = function (x) { return 'helper:' + x; };
let h = '';
for (let i = 0; i < 5; i++) h = fnHost.helper(i);
say('function.static', h);                   // 'helper:4'

// A function with NO box at all: `properties` stays undefined until something
// is written, so the arm's object-tag test fails and the call is the helper's
// TypeError rather than a crash or a stale slot.
function bare() { return 1; }
let bm = '';
try { bm = bare.missing(); } catch (e) { bm = e.name; }
say('bare.missing', bm);                     // 'TypeError'
