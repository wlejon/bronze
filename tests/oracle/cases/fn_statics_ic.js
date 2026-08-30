// A property read whose RECEIVER IS A FUNCTION — a class constructor, an
// ordinary function, an intrinsic — answered out of the function's statics box
// through the read site's inline cache (llvm_prop_get.cpp's statics arm,
// rt_prop_function.cpp's fill).
//
// The mechanism substitutes a DIFFERENT OBJECT for the receiver: a function's
// `static` members live in a side object hanging off the FunctionHeader, so the
// cached shape and slot describe that box and not the thing the source names.
// Every case below is a way that substitution could be spent where it is not
// the answer, and each one is warmed FIRST and disturbed AFTERWARDS, because a
// cold site proves nothing about a cache.
//
// Clause references are ECMA-262 (2024).

function say(label, value) { console.log(label + '=' + value); }

// --- 1. The warm own-static read, and the three ways it can change ----------
class A {
  static tag = 'A-tag';
  static count = 3;
  static hello() { return 'hi'; }
  static get computed() { return 'got-' + A.tag; }
}

// Warm the site: 5 identical reads through one source position.
let warm = '';
for (let i = 0; i < 5; i++) warm = A.tag;
say('warm.tag', warm);                       // 10.2's own data property: 'A-tag'
say('warm.count', A.count);                  // 3

// REASSIGNED. 10.2.9's box is an ordinary object, so this is an ordinary set
// on an existing own property: same shape, same slot, new value. A cache keyed
// on the shape must therefore still hit AND still see the new value.
A.tag = 'A-tag-2';
let after = '';
for (let i = 0; i < 5; i++) after = A.tag;
say('reassigned.tag', after);                // 'A-tag-2'

// ADDED. A new own property transitions the box to a NEW shape, so the warm
// entry stops describing it and the read falls to the helper, which refills.
A.extra = 'added';
say('added.extra', A.extra);                 // 'added'
say('added.tag', A.tag);                     // unchanged: 'A-tag-2'

// DELETED. 13.5.1.2 removes the own property; the box goes to dictionary mode,
// which every fill path refuses, so this must answer `undefined` rather than
// the slot the retired entry named.
delete A.extra;
say('deleted.extra', A.extra);               // undefined (7.3.3 Get on absent)
say('deleted.tag', A.tag);                   // still 'A-tag-2'

// --- 2. A static REDEFINED AS AN ACCESSOR mid-loop --------------------------
// The receiver a `static get` sees is the CLASS (15.7.14), not the box the
// getter is stored in — so an accessor must never be spent by the inline arm.
// Warmed as a data property first, then swapped underneath.
class B { static v = 'data'; }
let seen = '';
for (let i = 0; i < 4; i++) seen = B.v;
say('accessor.before', seen);                // 'data'
Object.defineProperty(B, 'v', { get() { return 'from-getter'; }, configurable: true });
let seenAfter = '';
for (let i = 0; i < 4; i++) seenAfter = B.v;
say('accessor.after', seenAfter);            // 'from-getter'

// A `static get` that reads `this`, warmed from cold. `this` must be the class.
class C {
  static base = 'C-base';
  static get derived() { return this.base + '!'; }
}
let g = '';
for (let i = 0; i < 4; i++) g = C.derived;
say('staticget.this', g);                    // 'C-base!' — receiver is C, not the box

// --- 3. INHERITED statics: `class D extends A` ------------------------------
// 15.7.14 sets D's [[Prototype]] to A, and bronze links D's statics box above
// A's. An inherited static is NOT an own property of D's box, so the arm must
// refuse it and the helper must still answer.
class D extends A {}
say('extends.own', D.name);                  // 'D' (10.2.9, from the header)
let inh = '';
for (let i = 0; i < 4; i++) inh = D.tag;
say('extends.inherited', inh);               // 'A-tag-2', through A's box
say('extends.method', D.hello());            // 'hi'

// A subclass that SHADOWS the inherited name with its own.
class E extends A { static tag = 'E-tag'; }
let sh = '';
for (let i = 0; i < 4; i++) sh = E.tag;
say('extends.shadowed', sh);                 // 'E-tag' — own wins over inherited

// --- 4. One site over a FUNCTION and a PLAIN OBJECT -------------------------
// The polymorphism the arm is most exposed to: a function's box and a plain
// object are both plain objects drawn from the same shape arena, and can hold
// the SAME shape pointer. That is sound precisely because a shape is a
// key-to-slot map and nothing more — but only if each receiver reads its own
// storage, which is what this checks.
function readTag(o) { return o.tag; }
class F { static tag = 'F-static'; }
const plain = { tag: 'plain-own' };
let mixed = '';
for (let i = 0; i < 6; i++) {
  mixed += readTag(F) + '/' + readTag(plain) + ' ';
}
say('mixed.fn.plain', mixed.trim());         // six repeats of 'F-static/plain-own'

// `A.prototype` vs `A.staticName` at the SAME source position. `prototype`
// lives in its own header slot (10.2.4), never in the box.
function readProp(o, which) { return which ? o.prototype : o.tag; }
say('proto.vs.static', typeof readProp(A, true) + ',' + readProp(A, false));
                                             // 'object,A-tag-2'

// --- 5. A function with NO statics box at all -------------------------------
// `properties` is undefined until something is written, so the arm's
// object-tag test fails and the read takes the helper. `undefined`, not a
// crash and not a stale slot.
function bare() { return 1; }
let b0 = '';
for (let i = 0; i < 4; i++) b0 = String(bare.missing);
say('bare.missing', b0);                     // 'undefined'
say('bare.name', bare.name);                 // 'bare' (10.2.9)
say('bare.length', bare.length);             // 0 (10.2.10)

// --- 6. `Function.prototype.call` through the same site as a static ---------
// `call` is inherited from %Function.prototype% (20.2.3.3) and is answered
// BELOW the box in the runtime's order, so it must miss the arm every time.
// The same site also sees a class whose own static IS named `call`, which
// 15.7.14 says shadows the inherited one.
function readCall(o) { return o.call; }
class G { static call = 'own-call'; }
say('call.inherited', typeof readCall(bare));  // 'function'
say('call.shadowed', readCall(G));             // 'own-call'
say('call.inherited.again', typeof readCall(bare));  // 'function' — not poisoned

// A BOUND function: 10.4.1 makes an exotic object with its own `name` and
// `length`, and no statics box of its own.
const boundFn = bare.bind(null);
say('bound.name', boundFn.name);             // 'bound bare' (10.4.1.3)
say('bound.length', boundFn.length);         // 0 (10.4.1.3 step 9)

// --- 7. INTRINSIC receivers, which the fill refuses by name -----------------
// %Object% carries a real statics box, so `Object.keys` is a genuine own
// property of it and the arm may cache it. %Array% and %Number% are answered
// by the constructor TABLE ahead of the box, so their fill is refused — a
// warm site must not start answering a user assignment where the language
// says the builtin.
function readKeys(o) { return o.keys; }
let k = '';
for (let i = 0; i < 4; i++) k = typeof readKeys(Object);
say('intrinsic.Object.keys', k);             // 'function'
say('intrinsic.Object.works', Object.keys({ a: 1, b: 2 }).join(','));  // 'a,b'

function readFrom(o) { return o.from; }
let fr = '';
for (let i = 0; i < 4; i++) fr = typeof readFrom(Array);
say('intrinsic.Array.from', fr);             // 'function' (22.1.2.1, from kCtors)

// Number's box is populated at intrinsic init AND Number is in the table, so
// it is the receiver that proves the refusal matters.
function readEps(o) { return o.EPSILON; }
let ep = 0;
for (let i = 0; i < 4; i++) ep = readEps(Number);
say('intrinsic.Number.EPSILON', ep === Math.pow(2, -52));  // true (21.1.2.1)

// --- 8. Depth: a static on a THREE-level class chain ------------------------
class H1 { static deep = 'deep-1'; }
class H2 extends H1 {}
class H3 extends H2 {}
let d = '';
for (let i = 0; i < 4; i++) d = H3.deep;
say('depth3.inherited', d);                  // 'deep-1' — two boxes up, via helper

// --- 9. The warm site survives a shape change on the OTHER receiver ---------
// Adding a property to `plain` retires its way but must leave F's alone.
plain.other = 1;
let post = '';
for (let i = 0; i < 4; i++) post = readTag(F) + '/' + readTag(plain);
say('post.mutation', post);                  // 'F-static/plain-own'
