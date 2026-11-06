// `Array.isArray(v)` answered at the call site while the site's method cache
// holds the constructor static: every kind of argument the inline test
// answers or hands on (arrays, class instances, literals, null-prototype
// objects, `Array.prototype` itself and objects made from it, proxies,
// typed arrays, functions, primitives), then the static is patched, deleted,
// restored, its constructor's statics are written without touching it, and
// the same site meets another receiver with an `isArray` of its own.
// Whatever the build guessed, the bytes are these.

const out = [];
const log = (...xs) => out.push(xs.join(" "));

class Material { constructor() { this.color = 1; } }
class MeshStandardMaterial extends Material { constructor() { super(); this.rough = 0.5; } }
class List extends Array {}

const bare = Object.create(null);
bare.k = 1;
const fromProto = Object.create(Array.prototype);
const literal = { a: 1 };
const proxied = new Proxy([1, 2], {});
const proxiedObj = new Proxy({}, {});
const values = [
  [1, 2, 3], [], new MeshStandardMaterial(), new Material(), literal, {}, bare, fromProto, Array.prototype,
  proxied, proxiedObj, new Float32Array(2), function () {}, () => 1, "str", 3, 2.5, undefined, null, true,
  Symbol("s"), new List(), new Map(), new Date(0),
];

function check(v) { return Array.isArray(v); }
function sweep() {
  let s = "";
  for (let i = 0; i < values.length; ++i) s += check(values[i]) ? "1" : "0";
  return s;
}

let warm = "";
for (let r = 0; r < 200; ++r) warm = sweep();
log("warm", warm);

// A material walk, the hot shape of the call.
function countArrays(list) {
  let n = 0;
  for (let i = 0; i < list.length; ++i) if (Array.isArray(list[i])) ++n;
  return n;
}
const mats = [];
for (let i = 0; i < 64; ++i) mats.push(i % 5 === 0 ? [new Material()] : new MeshStandardMaterial());
let c = 0;
for (let r = 0; r < 50; ++r) c = countArrays(mats);
log("materials", c);

// Statics written down, isArray left alone: the box now holds them.
Array.custom = 7;
log("materialized", sweep(), countArrays(mats));

// Patched, deleted, restored.
const original = Array.isArray;
Array.isArray = function (v) { return typeof v === "object" ? "obj" : "no"; };
log("patched", check([]), check(1), countArrays(mats));
delete Array.isArray;
try { check([]); log("deleted no throw"); } catch (e) { log("deleted", e.constructor.name); }
Array.isArray = original;
log("restored", sweep());
Object.defineProperty(Array, "isArray", { value: (v) => "defined", configurable: true, writable: true });
log("defined", check([]));
Array.isArray = original;
log("again", sweep());

// The same site, another receiver.
function viaRecv(R, v) { return R.isArray(v); }
let mixed = "";
const fake = { isArray(v) { return "fake:" + typeof v; } };
for (let r = 0; r < 100; ++r) mixed = viaRecv(Array, [1]) + "," + viaRecv(Array, literal) + "," + viaRecv(fake, 1);
log("receivers", mixed);
class Arr2 { static isArray(v) { return "static:" + Array.isArray(v); } }
log("class static", viaRecv(Arr2, []), viaRecv(Array, []));

// A revoked proxy throws from IsArray.
const r = Proxy.revocable([], {});
r.revoke();
try { check(r.proxy); log("revoked no throw"); } catch (e) { log("revoked", e.constructor.name); }

// Extra arguments are evaluated and ignored.
let evaluated = 0;
log("extras", Array.isArray([], ++evaluated, ++evaluated), Array.isArray({}, ++evaluated), evaluated);

console.log(out.join("\n"));
