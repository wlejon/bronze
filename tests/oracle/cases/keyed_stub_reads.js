// A computed read that never indexes an Array or a view and that no own
// slot answers: a constant of a prototype with hundreds of properties, a
// method by its name, a key that is absent, a dictionary. The site reads
// through the stub; its pair cell stops asking for a fill. Then the same
// sites meet an own slot, an Array and a typed array.
const C = {};
for (let i = 0; i < 300; i++) C['K' + i] = i * 3 + 1;
class Ctx {
  constructor() { this.calls = 0; }
  texParameteri(a, b, c) { this.calls++; return a + b + c; }
}
for (const k in C) Ctx.prototype[k] = C[k];
const table = { repeat: 10497, 'clamp-to-edge': 33071 };
function apply(gl, style, name, konst, first) {
  const s = table[style.u];
  const t = table[style.missing];
  let r = gl[name](first, gl[konst], s);
  if (t !== undefined) r += t;
  return r;
}
const gl = new Ctx();
const styles = [{ u: 'repeat', missing: 'nope' }, { u: 'clamp-to-edge', missing: 'nada' }];
let sum = 0;
for (let i = 0; i < 300000; i++) sum += apply(gl, styles[i & 1], 'texParameteri', i % 3 === 0 ? 'K150' : 'K7', 3553);
console.log(sum, gl.calls);

const dict = {};
for (let i = 0; i < 40; i++) dict['d' + i] = i;
delete dict.d3;
function read(o, k) { return o[k]; }
let acc = 0;
for (let i = 0; i < 200000; i++) acc += read(dict, 'd' + (i % 40)) | 0;
console.log(acc);
// The same site, other receivers and keys.
console.log(read({ own: 5 }, 'own'), read([7, 8, 9], 1), read(new Float32Array([1.5, 2.5]), 1), read('abc', 2),
            read(dict, 'd3'), read(gl, 'K299'), read(gl, 'calls'));
for (let i = 0; i < 2000; i++) acc += read([i, i + 1], i & 1);
console.log(acc);
