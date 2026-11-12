// A guarded region tests a value read off an object right behind the reads off
// that receiver, and from there on holds it as a double. A guard that fails
// later (a second receiver's field, an element further along) leaves for the
// slow copy with the earlier values rebuilt from their doubles. Each run below
// makes a late guard fail after earlier values were unboxed, with the earlier
// values being the numbers a rebuild could get wrong: NaN, -0, the infinities,
// the largest and smallest doubles. What the slow copy then computes with
// them, and stores of them, must be what the plain accesses give.
function show(v) {
  if (typeof v === 'number') return Object.is(v, -0) ? '-0' : String(v);
  if (typeof v === 'string') return JSON.stringify(v);
  if (typeof v === 'bigint') return v + 'n';
  return String(v);
}

// Two fields off `o`, then one off `p`, whose test is at its first use.
function mix(o, p, out) {
  const a = o.x;
  const b = o.y;
  const c = p.z;
  const d = a * b + c;
  out.a = a;
  out.b = b;
  out.d = d + a;
  return d;
}

// The same in a loop over receivers, with a running sum carried across the
// exit as well.
function sumAll(list, p) {
  let s = 0;
  let last = 0;
  for (let i = 0; i < list.length; i++) {
    const o = list[i];
    const a = o.x;
    const b = o.y;
    const c = p.z;
    s = s + a * b + c;
    last = a;
  }
  return [s, last];
}

// Element reads: all tested behind the reads, the last use of the first one
// past a second array's read.
function elems(e, f) {
  const a = e[0];
  const b = e[1];
  const c = e[2];
  const g = f[0];
  const h = f[1];
  const r = a * b + c * g;
  return [r + h, a, c, h];
}

// A matrix-style kernel: ten fields off two receivers, the last one bad.
function append(m, n, out) {
  const a1 = m.a, b1 = m.b, c1 = m.c, d1 = m.d, tx1 = m.tx, ty1 = m.ty;
  const a2 = n.a, b2 = n.b, c2 = n.c, d2 = n.d;
  out.a = a1 * a2 + b1 * c2;
  out.b = a1 * b2 + b1 * d2;
  out.c = c1 * a2 + d1 * c2;
  out.d = c1 * b2 + d1 * d2;
  out.tx = tx1 * a2 + ty1 * c2 + n.tx;
  out.ty = tx1 * b2 + ty1 * d2 + n.ty;
  out.first = a1;
  return out;
}

const numbers = [NaN, -0, 0, Infinity, -Infinity, 1.5, Number.MAX_VALUE, Number.MIN_VALUE, -1e-320];
const late = [2, 'str', undefined, null, true, { valueOf() { return 7; } }, '', [3], NaN, -0];

function attempt(f) {
  try {
    return f();
  } catch (err) {
    return 'threw ' + err.constructor.name;
  }
}

for (let warm = 0; warm < 3; warm++) {
  const lines = [];
  for (const x of numbers) {
    for (const z of late) {
      const out = {};
      const d = mix({ x, y: -0 }, { z }, out);
      lines.push(['mix', show(x), show(z), show(d), show(out.a), show(out.b), show(out.d)].join(' '));
      const out2 = {};
      const d2 = mix({ x: 2, y: x }, { z }, out2);
      lines.push(['mix2', show(d2), show(out2.a), show(out2.b), show(out2.d)].join(' '));
    }
  }
  for (const z of late) {
    const list = numbers.map((x) => ({ x, y: 1 }));
    const r = sumAll(list, { z });
    lines.push(['sum', show(z), show(r[0]), show(r[1])].join(' '));
    const tail = sumAll([{ x: -0, y: -0 }, { x: -0, y: 1 }], { z });
    lines.push(['tail', show(tail[0]), show(tail[1])].join(' '));
  }
  for (const x of numbers) {
    for (const z of late) {
      lines.push(['elems', elems([x, -0, x], [1, z]).map(show).join(',')].join(' '));
      lines.push(['elems2', elems([x, 1, -0], [z, x]).map(show).join(',')].join(' '));
      lines.push(['elems3', elems([x, x], [z, z]).map(show).join(',')].join(' '));
    }
  }
  for (const x of numbers) {
    for (const z of late) {
      const m = { a: x, b: -0, c: NaN, d: Infinity, tx: -0, ty: x };
      const n = { a: 1, b: -0, c: 0, d: -1, tx: -0, ty: z };
      const o = append(m, n, {});
      lines.push(['append', show(o.a), show(o.b), show(o.c), show(o.d), show(o.tx), show(o.ty), show(o.first)].join(' '));
      const n2 = { a: 1, b: -0, c: 0, d: z, tx: -0, ty: -0 };
      const o2 = append(m, n2, {});
      lines.push(['append2', show(o2.a), show(o2.b), show(o2.c), show(o2.d), show(o2.tx), show(o2.ty), show(o2.first)].join(' '));
    }
  }
  // A BigInt behind unboxed numbers: the slow copy's mix throws.
  lines.push(attempt(() => show(mix({ x: -0, y: NaN }, { z: 1n }, {}))));
  lines.push(attempt(() => show(mix({ x: 2n, y: 3n }, { z: 1n }, {}))));
  lines.push(attempt(() => elems([-0, NaN, Infinity], [1, 2n]).map(show).join(',')));
  // A getter on the second receiver that runs once, whichever copy reads it.
  let reads = 0;
  const p = { get z() { reads++; return reads % 2 ? 'odd' : -0; } };
  for (let i = 0; i < 4; i++) {
    const out = {};
    lines.push(['getter', show(mix({ x: -0, y: 5 }, p, out)), show(out.a), show(out.d), reads].join(' '));
  }
  if (warm === 2) console.log(lines.join('\n'));
  else console.log(lines.length);
}
