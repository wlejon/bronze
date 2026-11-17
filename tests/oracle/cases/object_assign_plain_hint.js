// Object.assign between plain objects replays a remembered copy (spread_plain.cpp).
// Each section warms that copy first, then changes something only the
// prototype chain or the target's own attributes can see.
const src = { a: 1, b: 2, c: 3 };
function copyMany(make) {
  let last;
  for (let i = 0; i < 50; i++) last = Object.assign(make(), src, { b: i });
  return last;
}
console.log(JSON.stringify(copyMany(() => ({}))));

// A setter appears on a prototype after the copy is remembered: Set runs it.
class Box { constructor() { this.z = 0; } }
console.log(JSON.stringify(copyMany(() => new Box())));
let seen = [];
Object.defineProperty(Box.prototype, 'a', { set(v) { seen.push(v); }, configurable: true });
const boxed = copyMany(() => new Box());
console.log(JSON.stringify(boxed), Object.keys(boxed).join(','), seen.length, seen[0]);

// A read-only property on a plain prototype refuses the add, as a TypeError.
const P = { base: true };
console.log(JSON.stringify(copyMany(() => Object.create(P))));
Object.defineProperty(P, 'c', { value: 9, writable: false, configurable: true });
try { copyMany(() => Object.create(P)); console.log('no throw'); } catch (e) { console.log(e instanceof TypeError); }
delete P.c;
console.log(JSON.stringify(copyMany(() => Object.create(P))));

// A target owning a key as non-enumerable keeps it non-enumerable.
const hidden = () => Object.defineProperty({}, 'a', { value: 0, writable: true, enumerable: false, configurable: true });
const h = copyMany(hidden);
console.log(h.a, Object.keys(h).join(','));

// A non-writable own key throws and stops the copy.
const ro = () => Object.defineProperty({}, 'b', { value: 0, writable: false, enumerable: true, configurable: true });
try { Object.assign(ro(), src); console.log('no throw'); } catch (e) { console.log(e instanceof TypeError); }

// A target with no prototype, and one whose prototype is a proxy.
console.log(JSON.stringify(copyMany(() => Object.create(null))));
const traps = [];
const viaProxy = Object.assign(Object.create(new Proxy({}, { set(t, k, v, r) { traps.push(k); return true; } })), src);
console.log(traps.join(','), Object.keys(viaProxy).length);

// Getters on the source run, and spread keeps defining over a setter.
let reads = 0;
const getterSrc = { get a() { reads++; return 7; }, b: 1 };
for (let i = 0; i < 20; i++) Object.assign({}, getterSrc);
console.log(reads);
Object.defineProperty(P, 'k', { set(v) { seen.push('k' + v); }, configurable: true });
const spreadK = { ...{ k: 1 } };
const assignK = Object.assign(Object.create(P), { k: 2 });
console.log(Object.keys(spreadK).join(','), Object.keys(assignK).join(','), seen[seen.length - 1]);
