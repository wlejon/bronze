// Reads whose receiver is written as a class's name — `C.K`, `C.prototype` —
// take an inline arm for a class constructor receiver: its statics object at
// the shape the site cached, or its header's `prototype`. Each section warms a
// site and then changes what the name holds or what the class holds, so the
// arm's answer has to agree with the ordinary read every time.

class Base {
    static K = 1;
    static shared = 'base';
}
class Mid extends Base {
    static K = 2;
}

function readK() { return Base.K; }
function readShared() { return Mid.shared; }
function readProto() { return Base.prototype; }

let n = 0;
for (let i = 0; i < 200; i++) n += readK();
console.log('1', n, readShared(), readProto() === Object.getPrototypeOf(new Base()));

// 2. The static rewritten, then made an accessor, then deleted.
Base.K = 5;
const a = readK();
Object.defineProperty(Base, 'K', { get() { return 'getter'; }, configurable: true });
const b = readK();
delete Base.K;
const c = readK();
Base.K = 6;
console.log('2', a, b, c, readK());

// 3. An inherited static through `extends`, then shadowed on the subclass.
let s = '';
for (let i = 0; i < 50; i++) s = readShared();
Mid.shared = 'mid';
const own = readShared();
delete Mid.shared;
console.log('3', s, own, readShared());

// 4. Frozen: the reads are reads.
Object.freeze(Base);
console.log('4', readK(), readProto() === Base.prototype, Object.isFrozen(Base));

// 5. A class binding reassigned: to an object with the same key, a function
// declaration with statics, an arrow (no `prototype`), and a primitive.
class Swap {
    static K = 'class';
}
function readSwap() { return Swap.K; }
function readSwapProto() { return typeof Swap.prototype; }
const out = [];
for (let i = 0; i < 100; i++) { readSwap(); readSwapProto(); }
out.push(readSwap(), readSwapProto());
Swap = { K: 'object', prototype: 'own' };
out.push(readSwap(), readSwapProto());
function Decl() {}
Decl.K = 'decl';
Swap = Decl;
out.push(readSwap(), readSwapProto());
Swap = () => 0;
out.push(String(readSwap()), readSwapProto());
Swap = 'str';
out.push(String(readSwap()), readSwapProto());
Swap = class { static K = 'second'; };
out.push(readSwap(), readSwapProto());
console.log('5', out.join(' '));

// 6. `name` and `length` come from the function unless a static defines them.
class Named { static K = 1; }
class Plain {}
class Meth { static name() { return 'm'; } }
function readName(C) { return typeof C.name + ':' + C.length; }
let names = '';
for (let i = 0; i < 30; i++) names = readName(Named) + ' ' + readName(Plain) + ' ' + readName(Meth);
console.log('6', names, Named.name, Plain.name, Meth.name());

// 7. A static holding a double that later holds an object, read in a loop.
class Num { static v = 0.5; }
let t = 0;
for (let i = 0; i < 100; i++) {
    t += Num.v;
    if (i === 50) Num.v = 2;
}
Num.v = { x: 1 };
console.log('7', t, Num.v.x);
