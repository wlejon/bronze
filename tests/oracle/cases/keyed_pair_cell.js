// A computed read that names one (shape, key) pair in the warm-up, then
// other pairs: other keys of the shape, other shapes with the key at another
// slot, keys built at run time (equal characters, another string), absent
// keys, accessors, inherited properties, and receivers that are no plain
// object. The site's cell holds one pair; every other read has to answer as
// the generic read does.
function read(o, k) {
    return o[k];
}
function sum(list, k) {
    let t = 0;
    for (let i = 0; i < list.length; i++) {
        const v = read(list[i], k);
        t += typeof v === 'number' ? v : v === undefined ? -1 : String(v).length;
    }
    return t;
}
const pipes = { sprite: 1, mesh: 2, text: 3 };
const items = [];
for (let i = 0; i < 64; i++) items.push({ id: 'sprite', pipes });
let total = 0;
for (let r = 0; r < 300; r++) {
    for (let i = 0; i < items.length; i++) total += read(items[i].pipes, items[i].id);
}
console.log(total);

// Other keys and shapes.
const other = { text: 30, sprite: 10 };
const withGetter = { get sprite() { return 77; } };
const child = Object.create(pipes);
const arr = [5, 6, 7];
arr.sprite = 9;
const made = ['spr', 'ite'].join('');
const keys = ['sprite', 'mesh', 'text', 'absent', made, 'spr' + 'ite', 0, 1, 'length'];
const recvs = [pipes, other, withGetter, child, arr, 'sprite', new Map(), function f() {}];
let mixed = 0;
for (let r = 0; r < 40; r++) {
    for (const k of keys) mixed += sum(recvs, k);
}
console.log(mixed);

// The pair again after a shape change of the receiver and a delete.
pipes.extra = 4;
total = 0;
for (let i = 0; i < items.length; i++) total += read(items[i].pipes, items[i].id);
delete pipes.sprite;
for (let i = 0; i < items.length; i++) total += read(items[i].pipes, items[i].id) === undefined ? 100 : 0;
pipes.sprite = 8;
for (let i = 0; i < items.length; i++) total += read(items[i].pipes, items[i].id);
console.log(total);

// A Number key naming a property, one object a reader as pixi's
// `sprite._gpuData[renderer.uid]`; then other numbers, -0, and an Array.
const uid = 7;
const sprites = [];
for (let i = 0; i < 64; i++) sprites.push({ _gpuData: { 7: { n: i } } });
function gpu(s, id) {
    return s._gpuData[id];
}
let nums = 0;
for (let r = 0; r < 300; r++) {
    for (let i = 0; i < sprites.length; i++) nums += gpu(sprites[i], uid).n;
}
sprites[3]._gpuData[8] = { n: 1000 };
sprites[5]._gpuData = [0, 1, 2, 3, 4, 5, 6, { n: 5000 }];
sprites[6]._gpuData = { 0: { n: 9 }, 7: { n: 70 } };
for (let i = 0; i < sprites.length; i++) nums += gpu(sprites[i], uid).n;
nums += gpu(sprites[3], 8).n + gpu(sprites[6], -0).n + (gpu(sprites[6], 7.5) === undefined ? 1 : 0);
console.log(nums);

// Keys that are fresh strings every time, through collections.
let fresh = 0;
for (let i = 0; i < 20000; i++) {
    const k = 'k' + (i & 3);
    const o = { k0: 1, k1: 2, k2: 3, k3: 4 };
    fresh += read(o, k);
    if ((i & 1023) === 0) fresh += read(pipes, 'spr' + 'ite'.slice(0));
}
console.log(fresh);
