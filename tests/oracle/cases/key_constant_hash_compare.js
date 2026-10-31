// Interned key constants carry their string hash (rtKeyHeader), so an
// inline === against one rejects an unequal string of the same length by
// hash. Equal strings built at run time, with no hash yet or a different
// representation, must still compare equal; unequal ones of the same
// length must not.

function cls(s) {
    switch (s) {
        case 'x': return 1;
        case 'y': return 2;
        case 'z': return 3;
        case 'width': return 4;
        case 'height': return 5;
        case 'position': return 6;
        default: return 0;
    }
}
function eq(s) {
    return (s === 'position' ? 'P' : '') + (s === 'rotation' ? 'R' : '') + (s !== 'scale' ? '' : 'S');
}

const inputs = ['x', 'y', 'z', 'w', 'X', 'width', 'widtH', 'height', 'heighu', 'position',
                'positioN', 'rotation', 'rotatioN', 'scale', 'scalf', 'scal', '',
                'x' + '', ['wid', 'th'].join(''), 'posi' + 'tion', 'rota'.concat('tion'),
                String.fromCharCode(115, 99, 97, 108, 101), 'hei'.padEnd(6, 'ght').slice(0, 6),
                JSON.parse('"height"'), 'position'.split('').reverse().reverse().join(''),
                1, null, undefined, {}, Symbol.iterator];
const out = [];
for (let rep = 0; rep < 3; rep++) {
    for (const s of inputs) out.push(typeof s === 'symbol' ? 'sym' : cls(s) + eq(s));
}
console.log(out.join(' '));

// The same strings as object keys and through a Map.
const o = {};
for (const s of inputs) if (typeof s === 'string') o[s] = (o[s] || 0) + 1;
console.log(Object.keys(o).map(k => k + ':' + o[k]).join(' '));
const m = new Map();
for (const s of inputs) m.set(s, cls(s));
console.log(m.size, m.get('position'), m.get('posi' + 'tion'));
