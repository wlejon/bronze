// Computed reads `o[k]` over many (object layout, key) pairs at once, so
// that pairs share entries of the computed-read cache: every read answers
// from its own object, for own keys, inherited keys and absent keys, as the
// objects grow, lose keys and change prototypes.
const keys = [];
for (let i = 0; i < 48; i++) keys.push('k' + i);
const protoA = { inheritedA: 'pa', shared: 'from A' };
const protoB = { inheritedB: 'pb', shared: 'from B' };
const objects = [];
for (let i = 0; i < 160; i++) {
    const o = Object.create(i % 2 === 0 ? protoA : protoB);
    // A layout of its own per object: a different run of keys each.
    for (let j = 0; j < 6; j++) { const k = keys[(i * 7 + j * 5) % keys.length]; o[k] = i * 100 + j; }
    objects.push(o);
}
function read(o, k) { return o[k]; }
function tally() {
    let sum = 0, absent = 0, inherited = 0;
    for (let round = 0; round < 30; round++) {
        for (let i = 0; i < objects.length; i++) {
            const o = objects[i];
            for (let j = 0; j < keys.length; j += 5) {
                const v = read(o, keys[(j + round) % keys.length]);
                if (v === undefined) absent++; else sum += v;
            }
            if (read(o, 'shared') === 'from A') inherited++;
            if (read(o, i % 2 === 0 ? 'inheritedA' : 'inheritedB') !== undefined) inherited++;
            if (read(o, i % 2 === 0 ? 'inheritedB' : 'inheritedA') === undefined) absent++;
        }
    }
    return sum + ' ' + absent + ' ' + inherited;
}
console.log(tally());
// Keys from for-in, read back from three objects of different layouts, the
// way a renderer compares attribute tables.
const program = { position: { location: 0 }, normal: { location: 1 }, uv: { location: 2 }, skinIndex: { location: 3 }, skinWeight: { location: -1 } };
const cached = { position: 'cp', normal: 'cn', uv: 'cu', skinIndex: 'ci' };
const geometry = { index: 0, position: 'cp', uv: 'cu', normal: 'cn', skinIndex: 'other' };
function compare() {
    let same = 0, differ = 0;
    for (const name in program) {
        const p = program[name];
        if (p.location >= 0) {
            const c = cached[name], g = geometry[name];
            if (c === g) same++; else differ++;
        }
    }
    return same * 10 + differ;
}
let cmp = 0;
for (let i = 0; i < 4000; i++) cmp += compare();
console.log(cmp);
// The answers follow the objects: a key added, a key deleted, a prototype's
// key shadowed, a prototype replaced.
objects[3].shared = 'own';
delete objects[4][Object.keys(objects[4])[0]];
protoA.late = 'late A';
Object.setPrototypeOf(objects[5], protoA);
objects[6].k0 = 'new k0';
console.log(tally());
console.log(read(objects[3], 'shared'), read(objects[4], Object.keys(objects[4])[0]), read(objects[2], 'late'), read(objects[5], 'late'), read(objects[5], 'inheritedB'), read(objects[6], 'k0'));
// Number and boolean keys name properties too.
const byId = { 2884: 'a', 2929: 'b', 2960: 'c', true: 't', false: 'f' };
const other = { 2929: 'B', 2960: 'C', 32823: 'D', false: 'F' };
let ids = '';
for (let i = 0; i < 3000; i++) {
    const id = i % 3 === 0 ? 2884 : i % 3 === 1 ? 2929 : 2960;
    const a = read(byId, id), b = read(other, id), c = read(i % 2 ? byId : other, i % 4 < 2);
    if (i >= 2994) ids += a + (b === undefined ? '-' : b) + c + ' ';
}
console.log(ids);
