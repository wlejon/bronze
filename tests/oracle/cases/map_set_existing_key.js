// Map.prototype.set on a key the map already holds: the value changes in
// place, the entry keeps its position, and the call returns the map.
const m = new Map([[3317, 4], [3333, 4], [37440, false], ['name', 'a'], [NaN, 1], [0, 'zero']]);
const obj = { id: 1 }, sym = Symbol('s');
m.set(obj, 'o').set(sym, 'y').set(true, 't').set(null, 'n').set(undefined, 'u').set(10n, 'big');
const show = () => console.log(m.size, [...m].map(([k, v]) => String(typeof k === 'symbol' ? 'sym' : typeof k === 'object' && k ? 'obj' : k) + '=' + String(v)).join(' '));
show();

// Overwrites, by every kind of key, in a loop long enough to be compiled.
let chained = 0;
for (let i = 0; i < 3000; i++) {
    if (m.set(3317, i) === m) chained++;
    m.set(37440, i % 2 === 0);
    m.set('name', 'n' + (i % 7));
    m.set(NaN, i * 2);
    m.set(-0, i + 0.5);            // -0 is the key 0
    m.set(obj, i).set(sym, -i);
    m.set(true, i).set(null, i).set(undefined, i).set(10n, i);
}
console.log(chained);
show();
console.log(m.get(0), m.get(-0), m.get(NaN), m.get(obj), m.get(sym), m.get(10n), m.has(3333));

// A string key equal by characters, not by identity.
const built = ['na', 'me'].join('');
m.set(built, 'joined');
console.log(m.get('name'), m.size);

// Values that are objects, and a value that is the map itself.
const v1 = { tag: 'v1' }, v2 = [1, 2];
m.set(3333, v1); m.set(3333, v2); m.set('name', m);
console.log(m.get(3333) === v2, m.get('name') === m, m.size);

// Delete, set again: the key moves to the end; overwrite keeps it there.
m.delete(3317);
m.set(3317, 'back');
m.set(3317, 'again');
console.log([...m.keys()].slice(-2).map(String).join(), m.get(3317));

// After a clear the first set inserts.
m.clear();
m.set(1, 'a'); m.set(1, 'b');
console.log(m.size, m.get(1));

// Set.prototype.add of a present element, and a subclass.
const s = new Set([1, 'x', obj]);
for (let i = 0; i < 100; i++) s.add(1).add('x').add(obj);
console.log(s.size, [...s].length);
class Counted extends Map {
    set(k, v) { this.writes = (this.writes || 0) + 1; return super.set(k, v); }
}
const c = new Counted();
for (let i = 0; i < 50; i++) c.set('k', i);
console.log(c.writes, c.get('k'), c.size);

// Not a map.
try { Map.prototype.set.call({}, 1, 2); } catch (e) { console.log(e instanceof TypeError); }
try { Map.prototype.set.call(new Set(), 1, 2); } catch (e) { console.log(e instanceof TypeError); }
// One argument: the value is undefined.
const one = new Map([[1, 'x']]);
one.set(1);
console.log(one.get(1), one.has(1), one.size);
