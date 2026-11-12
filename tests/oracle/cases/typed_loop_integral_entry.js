// Loops over Arrays and views whose indices are sums, differences and
// constant multiples of a counter, started at values the loop does not know
// (lower/typed_loop_index_proof.cpp). The versioned copy skips each index's
// integer test behind one test of the starting values on entry, so a start
// that is a fraction, NaN, an infinity or past 2^53 must take the loop that
// still asks.
function pack(positions, uvs, f32, u32, index, offset, count, color) {
    const end = offset + count;
    for (let i = offset; i < end; i++) {
        const i2 = i * 2;
        const x = positions[i2];
        const y = positions[i2 + 1];
        f32[index++] = 2 * x + 3 * y + 1;
        f32[index++] = 5 * y - x;
        f32[index++] = uvs[i2];
        f32[index++] = uvs[i2 + 1];
        u32[index++] = color;
        u32[index++] = i;
    }
    return index;
}

function packIndex(indices, out, index, indexOffset, size, base) {
    for (let i = 0; i < size; i++) {
        out[index++] = base + indices[i + indexOffset];
    }
    return index;
}

function triples(verts, start, count, out) {
    let kept = 0;
    for (let i = start; i < count + start - 2; ++i) {
        const x0 = verts[i * 2];
        const y0 = verts[i * 2 + 1];
        const x1 = verts[(i + 1) * 2];
        const y1 = verts[(i + 1) * 2 + 1];
        const x2 = verts[(i + 2) * 2];
        const y2 = verts[(i + 2) * 2 + 1];
        if (Math.abs(x0 * (y1 - y2) + x1 * (y2 - y0) + x2 * (y0 - y1)) < 0.5) continue;
        out.push(i, i + 1, i + 2);
        kept++;
    }
    return kept;
}

function backwards(arr, from) {
    let sum = 0;
    for (let i = 1; i < 6; ++i) {
        sum += arr[(i - 1) * 2 + from] - arr[from - i];
    }
    return sum;
}

function show(label, v) {
    console.log(label + ": " + (ArrayBuffer.isView(v) ? Array.from(v).join(",") : JSON.stringify(v)));
}

const positions = [];
const uvs = [];
for (let i = 0; i < 24; i++) {
    positions.push(i * 1.5);
    uvs.push(i / 4);
}
const starts = [0, 6, 2.5, -6, NaN, Infinity, -Infinity, 9007199254740992, 1e300, -0, "6", 4294967296];
for (const start of starts) {
    for (const offset of [0, 3, 0.5, 20, -1, NaN, 2 ** 53 + 2]) {
        const buffer = new ArrayBuffer(96);
        const f32 = new Float32Array(buffer);
        const u32 = new Uint32Array(buffer);
        const next = pack(positions, uvs, f32, u32, start, offset, 2, 0xff00ff00);
        show("pack " + String(start) + " " + String(offset) + " -> " + String(next), u32);
    }
}

// The same loop over plain Arrays in place of the views, and Arrays with a
// hole, a getter and an own fractional key.
const plain = [];
const plain2 = [];
show("plain", [pack(positions, uvs, plain, plain2, 1, 2, 2, 7), plain, plain2]);
const odd = [];
odd[2.5] = "fraction";
show("fraction-key", [pack(positions, uvs, odd, odd, 2.5, 1, 1, 7), Object.keys(odd)]);
const holes = [1, , 3, 4, , 6, 7, 8];
show("holes", pack(holes, holes, new Float32Array(12), new Uint32Array(12), 0, 0, 2, 1));

const indices = [0, 1, 2, 2, 3, 0, 5, 4, 3];
for (const [index, indexOffset] of [[0, 0], [2, 3], [0.5, 0], [0, 0.5], [3, -2], [NaN, 0], [0, NaN], [1e17, 1], [0, 2 ** 53]]) {
    const out16 = new Uint16Array(12);
    const out32 = new Uint32Array(12);
    const outArr = [];
    show("index16 " + index + " " + indexOffset + " -> " + packIndex(indices, out16, index, indexOffset, 4, 100), out16);
    show("index32 " + index + " " + indexOffset + " -> " + packIndex(indices, out32, index, indexOffset, 4, 100), out32);
    show("indexArr " + index + " " + indexOffset + " -> " + packIndex(indices, outArr, index, indexOffset, 4, 100), outArr);
}

const verts = [];
for (let i = 0; i < 40; i++) verts.push((i * 7) % 11, (i * i) % 7);
for (const start of [0, 4, 2.5, -1, NaN, 15.5, 2 ** 31]) {
    const out = [];
    show("triples " + start + " -> " + triples(verts, start, 8, out), out);
}
for (const from of [0, 4, 0.5, -3, NaN]) show("backwards " + from, String(backwards(positions, from)));
