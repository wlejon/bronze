// A HOLE is not an own property, so the array methods' HasProperty and Get of
// its index go on up the prototype chain (10.1.7.1, 10.1.8.1) — and what they
// find there is what 23.1.3 moves, visits and compares. An element inherited
// into a hole is moved DOWN into an own element by `shift`, `splice`,
// `unshift`, `reverse` and `copyWithin`; read by `pop`, `includes`, `join`,
// `at` and `find`; and visited by `indexOf` and `forEach` because HasProperty
// says it is there. `fill` reads nothing and writes over the hole.
//
// `includes` and `indexOf` part company on a hole the chain does NOT answer:
// `includes` reads every index with Get and finds `undefined` there; `indexOf`
// asks HasProperty first and skips it.
//
// The chain is reached two ways: a prototype between the array and
// `Array.prototype` (Object.setPrototypeOf, and a subclass's prototype), and
// `Object.prototype`, which every array's chain ends at.

const withProto = (arr, indexed) => {
    const p = Object.assign(Object.create(Array.prototype), indexed);
    Object.setPrototypeOf(arr, p);
    return arr;
};
// The own keys, and each index's own value or a mark for a hole: what a
// method left BEHIND, which a read through the chain could not show.
const own = (a) => {
    const vals = [];
    for (let i = 0; i < a.length; i++) {
        vals.push(Object.prototype.hasOwnProperty.call(a, i) ? String(a[i]) : '_');
    }
    return Object.keys(a).join(',') + ' [' + vals.join(' ') + ']';
};

// 1. The movers.
let a = withProto([, 2, 3], { 0: 'p' });
console.log('1 shift', a.shift(), own(a));
a = withProto([1, , 3], { 1: 'p' });
console.log('1 shift middle', a.shift(), own(a));
a = withProto([1, 2, ,], { 2: 'p' });
console.log('1 pop', a.pop(), a.length, own(a));
a = withProto([, 2], { 0: 'p' });
console.log('1 unshift', a.unshift('u'), own(a));
a = withProto([, 2, 3], { 0: 'p' });
console.log('1 splice', a.splice(0, 1).join(), own(a));
a = withProto([1, , 3, 4], { 1: 'p' });
console.log('1 splice insert', a.splice(1, 0, 'i').length, own(a));
a = withProto([, 2, 3], { 0: 'p' });
a.reverse();
console.log('1 reverse', own(a));
a = withProto([, 2, 3], { 0: 'p' });
a.copyWithin(1, 0, 1);
console.log('1 copyWithin', own(a));
a = withProto([, 2, 3], { 0: 'p' });
a.fill(9, 1);
console.log('1 fill', own(a));

// 2. The readers and visitors.
a = withProto([, 2, 3, ,], { 0: 'p', 3: 'q' });
console.log('2', a.indexOf('p'), a.lastIndexOf('q'), a.includes('q'), a.join('-'), a.at(0), a.at(-1),
            a.find(x => x === 'p'), a.findLastIndex(x => x === 'q'));
a.forEach((v, i) => console.log('2 forEach', i, v));
console.log('2', a.map(x => x + '!').join(), a.filter(() => true).join(), a.slice(0, 2).join(),
            a.reduce((acc, x) => acc + x, ''), a.some(x => x === 'q'), a.every(x => x !== undefined),
            [].concat(a).join(), String(a));
a = withProto([, 'b', , 'a'], { 0: 'z' });
a.sort();
console.log('2 sort', own(a));

// 3. includes against indexOf on a hole nothing answers.
const plain = [1, , 3];
console.log('3', plain.includes(undefined), plain.indexOf(undefined), plain.join(), plain.at(1));

// 4. A getter on the chain runs with the ARRAY as `this`.
const g = [, 'x'];
const gp = Object.create(Array.prototype);
Object.defineProperty(gp, 0, { get() { return 'len' + this.length; }, configurable: true });
Object.setPrototypeOf(g, gp);
console.log('4', g.includes('len2'), g.indexOf('len2'), g.join(), g.at(0), own(g));
g.forEach((v, i) => console.log('4 forEach', i, v));

// 5. A subclass's prototype holding an index.
class Sub extends Array {}
Sub.prototype[1] = 's';
const s = new Sub();
s.length = 3;
s[0] = 'x';
console.log('5', s.join('|'), s.includes('s'), s.indexOf('s'), s.pop(), s.pop(), s.length);

// 6. Object.prototype, at the end of every array's chain, for an array that has
// no prototype of its own — and a shift in a hot function, where a compiled
// fast path would otherwise take the block as it stands.
function take(q) { return q.shift(); }
const round = () => {
    const out = [];
    for (let i = 0; i < 50; i++) {
        const q = [1, , 3];
        out.push(take(q) + ':' + own(q));
    }
    return out[0] + ' ' + out[49];
};
console.log('6 before', round());
Object.prototype[1] = 'o';
console.log('6 during', round());
const h = [1, ,];
console.log('6', h.includes('o'), h.indexOf('o'), h.join(), [, , 7].pop(), [5, ,].pop(), own(h));
delete Object.prototype[1];
console.log('6 after', round());
