// Computed reads o[k] on plain objects, which remember the receiver shape, key
// and slot they last found. Partway: a new key, a receiver of another shape,
// the slot's property deleted, a getter defined over it, the key moved to the
// prototype, a numeric-looking key and a symbol.
function read(list, keys) {
  let out = [];
  for (let i = 0; i < list.length; i++) out.push(list[i][keys[i % keys.length]]);
  return out.join(',');
}
function mk(a, b) { return { a, b, c: a + b }; }
const objs = [];
for (let i = 0; i < 20; i++) objs.push(mk(i, i * 2));
let t = 0;
for (let r = 0; r < 3000; r++) t += read(objs, ['b']).length;
console.log('warm', t);
console.log(read(objs.slice(0, 4), ['c']));
const other = { z: 1, b: 'other-b' };
console.log(read([objs[0], other, objs[1]], ['b']));
const del = mk(5, 6); delete del.b;
console.log(read([objs[2], del, objs[3]], ['b']));
const get = mk(7, 8);
let calls = 0;
Object.defineProperty(get, 'b', { get() { calls++; return 'getter'; } });
console.log(read([objs[4], get, objs[5], get], ['b']), calls);
const proto = { b: 'from-proto' };
const child = Object.create(proto); child.a = 1;
console.log(read([objs[6], child, objs[7]], ['b']));
proto.b = 'changed-proto';
console.log(read([child, objs[8]], ['b']));
const num = mk(1, 2); num['7'] = 'seven';
const sym = Symbol('s'); const so = mk(3, 4); so[sym] = 'sym';
console.log(read([num, objs[9]], ['7']), read([so, num], [sym]), read([objs[10], so], ['b']));
