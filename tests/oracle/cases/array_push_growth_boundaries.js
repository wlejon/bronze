// push across every growth boundary keeps every element, the length, and the
// holes a jump leaves.
const a = [];
let sum = 0;
for (let i = 0; i < 5000; i++) {
    if (a.push(i) !== i + 1) throw new Error('length ' + i);
    sum += i;
}
console.log(a.length, a[0], a[1], a[15], a[16], a[17], a[4999], a.reduce((x, y) => x + y, 0) === sum);

// Rebuilding a list by pushing survivors, as an event emitter does on removal.
let listeners = [];
for (let i = 0; i < 40; i++) listeners.push({ id: i });
for (let round = 0; round < 30; round++) {
    const kept = [];
    for (let i = 0; i < listeners.length; i++) if (listeners[i].id !== round) kept.push(listeners[i]);
    kept.push({ id: 100 + round });
    listeners = kept;
}
console.log(listeners.length, listeners.map((l) => l.id).join(','));

// Mixed element kinds, multi-argument push, and push after shrinking.
const mixed = [];
mixed.push(1, 'two', 3.5, null, undefined, { six: 6 }, [7]);
mixed.push();
for (let i = 0; i < 20; i++) mixed.push(i % 2 ? i : String(i));
console.log(mixed.length, JSON.stringify(mixed));
mixed.length = 3;
mixed.push('after');
console.log(JSON.stringify(mixed), mixed.length);
const popped = [];
for (let i = 0; i < 100; i++) popped.push(i);
for (let i = 0; i < 90; i++) popped.pop();
for (let i = 0; i < 50; i++) popped.push(-i);
console.log(popped.length, popped[9], popped[10], popped[59]);

// Holes and a far index.
const sparse = [];
sparse[20] = 'far';
sparse.push('next');
console.log(sparse.length, 5 in sparse, sparse[20], sparse[21], Object.keys(sparse).join());
const doubles = [];
for (let i = 0; i < 70; i++) doubles.push(i + 0.5);
doubles.push('str');
console.log(doubles.length, doubles[0], doubles[69], doubles[70]);

// Array-likes and frozen arrays.
const like = { length: 2, 0: 'a', 1: 'b' };
console.log(Array.prototype.push.call(like, 'c', 'd'), JSON.stringify(like));
try { Object.freeze([1]).push(2); } catch (e) { console.log(e instanceof TypeError); }
