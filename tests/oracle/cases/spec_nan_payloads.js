// NaNs with payloads (and the sign bit set) read from a Float64Array and a
// DataView, boxed into objects and arrays, stored back, compared, and
// computed with: every one is the one NaN to the program.
const buf = new ArrayBuffer(64);
const dv = new DataView(buf);
const f64 = new Float64Array(buf);
const u32 = new Uint32Array(buf);
const words = [[0x7ff80000, 1], [0xfff80000, 0], [0x7ff00000, 5], [0xfff12345, 0x6789], [0xffffffff, 0xffffffff], [0x7ff80000, 0], [0, 0], [0x40000000, 0]];
for (let i = 0; i < words.length; i++) { u32[2 * i + 1] = words[i][0]; u32[2 * i] = words[i][1]; }
function scan(rounds) {
  let nans = 0, eq = 0, kept = [], obj = { v: 0 };
  for (let r = 0; r < rounds; r++) {
    for (let i = 0; i < 8; i++) {
      const x = f64[i];
      const y = dv.getFloat64(i * 8, true);
      obj.v = x;
      kept[i] = y;
      if (x !== x) nans++;
      if (x === y) eq++;
      if (Number.isNaN(obj.v * 2 - y)) nans++;
    }
  }
  return [nans, eq, kept.map(String).join(), typeof obj.v, Object.is(kept[3], NaN)];
}
let last;
for (let r = 0; r < 2000; r++) last = scan(2);
console.log(String(last));
const g = new Float64Array(8);
for (let i = 0; i < 8; i++) { g[i] = f64[i]; dv.setFloat64(i * 8, f64[i] * 1, true); }
console.log(Array.from(g).map(String).join(), Array.from(f64).map(String).join());
const m = new Map([[f64[1], 'a']]);
console.log(m.get(NaN), m.get(f64[3]), [f64[4]].includes(NaN), [f64[4]].indexOf(NaN), Object.is(f64[0], f64[3]));
console.log(JSON.stringify([f64[2], -f64[1]]), String(Infinity - Infinity), Math.max(f64[5], 1), Math.min(1, f64[3]));
