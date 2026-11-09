import { Point, Node } from './base.js';
import { View, Leaf as MidLeaf } from './mid.js';

// A class two files below the accessors' declarer: `s.x` on it resolves
// through View to Node's accessor (a heritage named in another file).
class Sprite extends View {
    constructor(i) { super('s' + i); this.vx = (i % 3) - 1; this.vy = (i % 5) - 2; }
}

// The same source name as mid.js's Leaf, with other accessors.
class Leaf extends Node {
    get x() { return -this.position.x; }
    set x(v) { this.position.x = -v; }
}

class Override extends Sprite {
    get x() { return 'override ' + this.position.x; }
    set x(v) { this.position.x = v * 2; }
}

function step(list) {
    let sum = 0;
    for (let i = 0; i < list.length; i++) {
        const s = list[i];
        s.x += s.vx;
        s.y += s.vy;
        if (s.x > 5) { s.vx = -s.vx; s.x = 5; }
        else if (s.x < -5) { s.vx = -s.vx; s.x = -5; }
        sum += typeof s.x === 'number' ? s.x : s.x.length;
    }
    return sum;
}

function readAll(list) {
    const out = [];
    for (const s of list) out.push(String(s.x) + '/' + String(s.y));
    return out.join(' ');
}

const sprites = [];
for (let i = 0; i < 8; i++) sprites.push(new Sprite(i));
let total = 0;
for (let f = 0; f < 200; f++) total += step(sprites);
console.log('warm', total, readAll(sprites));
console.log('ticks', sprites.map((s) => s.ticks).join(','), sprites[0].position.writes);
console.log(sprites[2].tag(), sprites[3].describe(), new Point(3, -4).norm());

// Receivers that are not what the warm loop saw, through the same sites.
const own = new Sprite(9);
Object.defineProperty(own, 'x', { value: 42, writable: true, enumerable: true, configurable: true });
const odd = [new Leaf(), new Override(1), own, { x: 'plain', y: 7, vx: 1, vy: 1 }];
for (const o of odd) { o.vx = 1; o.vy = 1; }
for (let f = 0; f < 3; f++) console.log('odd', step(odd), readAll(odd));
// mid.js's Leaf has a getter and no setter: a module's store throws.
const getterOnly = new MidLeaf();
getterOnly.vx = 1; getterOnly.vy = 1;
try { step([getterOnly]); } catch (e) { console.log('getter only', e instanceof TypeError, getterOnly.x, getterOnly.ticks); }
console.log('own', own.x, own.position.x, own.ticks);

// The accessor redefined on the declaring class, and a prototype replaced.
Object.defineProperty(Node.prototype, 'x', {
    get() { return 'redefined'; },
    set(v) { this.position.x = v + 100; },
    configurable: true,
});
console.log('redefined', step(sprites), readAll(sprites.slice(0, 3)));
const swapped = new Sprite(4);
Object.setPrototypeOf(swapped, { get x() { return 7; }, set x(v) { this.seen = v; }, y: 1 });
swapped.vx = 1; swapped.vy = 1;
console.log('swapped', step([swapped]), swapped.seen, swapped.y);
console.log('ticks', sprites.map((s) => s.ticks).join(','));
