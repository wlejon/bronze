// Adversarial profile perturbations for bronze's oracle suite.
// Ported from tests/oracle/run.py: takes a real harvest (.tfb) and mutates it
// four different ways while keeping the format and digest valid so the
// optimiser's speculation + deopt guards are stressed against wrong profiles.

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
    t[i] = c >>> 0;
  }
  return t;
})();

export function crc32(str) {
  let c = 0xffffffff;
  for (let i = 0; i < str.length; i++) {
    c = CRC_TABLE[(c ^ (str.charCodeAt(i) & 0xff)) & 0xff] ^ (c >>> 8);
  }
  return (c ^ 0xffffffff) >>> 0;
}

export class Rng {
  constructor(seed) {
    this.state = (seed >>> 0) || 0x9e3779b9;
  }
  nextU32() {
    let t = (this.state = (this.state + 0x6d2b79f5) >>> 0);
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return (t ^ (t >>> 14)) >>> 0;
  }
  random() {
    return this.nextU32() / 4294967296;
  }
  randint(lo, hi) {
    return lo + (this.nextU32() % (hi - lo + 1));
  }
  choice(arr) {
    return arr[this.nextU32() % arr.length];
  }
}

export function splitLines(text) {
  if (!text) return [];
  const norm = text.replace(/\r\n/g, '\n').replace(/\r/g, '\n');
  const lines = norm.split('\n');
  if (lines.length > 0 && lines[lines.length - 1] === '') lines.pop();
  return lines;
}

export class Harvest {
  constructor(text) {
    this.rows = [];
    this.slowsites = null;
    this.slow = new Map();
    this.last = new Map();
    this.callee = new Map();
    this.ics = new Map();
    this.held = { doubles: new Map(), objects: new Map() };
    this.other = { mixed: [], nonobject: [] };
    this.functions = [];

    for (const line of splitLines(text)) {
      const f = line.split('\t');
      const k = f[0];
      if (k === 'slowsites') {
        this.slowsites = f.slice(1);
      } else if (k === 'slow') {
        this.slow.set(parseInt(f[1], 10), parseInt(f[2], 10));
      } else if (k === 'last') {
        this.last.set(parseInt(f[1], 10), f[2]);
      } else if (k === 'callee') {
        this.callee.set(parseInt(f[1], 10), f[2]);
      } else if (k === 'ic') {
        this.ics.set(parseInt(f[1], 10), {
          method: f[2] === 'method',
          flags: f[3] === '-' ? [] : f[3].split(','),
          ways: [],
          other: 0,
        });
      } else if (k === 'way') {
        this.ics.get(parseInt(f[1], 10)).ways.push(f.slice(2));
      } else if (k === 'other') {
        this.ics.get(parseInt(f[1], 10)).other = parseInt(f[2], 10);
      } else if (k === 'doubles' || k === 'objects') {
        this.held[k].set(f[1], f[2].split(',').map(x => parseInt(x, 10)));
      } else if (k === 'mixed' || k === 'nonobject') {
        this.other[k].push(f[1]);
      } else {
        this.rows.push(line);
      }
    }
  }

  armed() {
    if (!this.slowsites) return [];
    const n = parseInt(this.slowsites[0], 10);
    const bits = this.slowsites[1];
    const out = [];
    for (let i = 0; i < n; i++) {
      const w = Math.floor(i / 64) * 16;
      const word = BigInt('0x' + bits.slice(w, w + 16));
      if (((word >> BigInt(i % 64)) & 1n) !== 0n) out.push(i);
    }
    return out;
  }

  arm(i) {
    if (!this.slowsites) return;
    const n = parseInt(this.slowsites[0], 10);
    if (i >= n) return;
    const bits = this.slowsites[1];
    const w = Math.floor(i / 64) * 16;
    const word = BigInt('0x' + bits.slice(w, w + 16)) | (1n << BigInt(i % 64));
    this.slowsites[1] = bits.slice(0, w) + word.toString(16).padStart(16, '0') + bits.slice(w + 16);
  }

  text() {
    const out = this.rows.slice();
    if (this.slowsites) {
      out.push('slowsites\t' + this.slowsites.join('\t'));
      const n = parseInt(this.slowsites[0], 10);
      for (const i of Array.from(this.slow.keys()).sort((a, b) => a - b)) {
        const c = this.slow.get(i);
        if (c > 0 && i < n) out.push(`slow\t${i}\t${c}`);
      }
      for (const i of Array.from(this.last.keys()).sort((a, b) => a - b)) {
        if (i < n) out.push(`last\t${i}\t${this.last.get(i)}`);
      }
      for (const i of Array.from(this.callee.keys()).sort((a, b) => a - b)) {
        if (i < n) out.push(`callee\t${i}\t${this.callee.get(i)}`);
      }
    }
    for (const i of Array.from(this.ics.keys()).sort((a, b) => a - b)) {
      const s = this.ics.get(i);
      out.push(`ic\t${i}\t${s.method ? 'method' : 'property'}\t${s.flags.join(',') || '-'}`);
      for (const w of s.ways) out.push(['way', String(i), ...w].join('\t'));
      if (s.other) out.push(`other\t${i}\t${s.other}`);
    }
    for (const [held, other] of [['doubles', 'mixed'], ['objects', 'nonobject']]) {
      for (const r of Array.from(this.held[held].keys()).sort()) {
        const p = this.held[held].get(r);
        if (p && p.length > 0) out.push(`${held}\t${r}\t${p.join(',')}`);
      }
      for (const k of Array.from(new Set(this.other[other])).sort()) {
        out.push(`${other}\t${k}`);
      }
    }
    return out.join('\n') + '\n';
  }
}

function keysOf(name) {
  const m = /^([^{}]*)\{([^{}]*)\}$/.exec(name);
  if (!m || m[2] === 'dict') return null;
  return [m[1], m[2] ? m[2].split(',') : []];
}

function reshape(name, r) {
  const parts = keysOf(name);
  if (!parts) return name;
  let [head, keys] = parts;
  const pick = r.randint(0, 4);
  if (pick === 0) {
    keys = keys.slice(1).concat(keys.slice(0, 1));
  } else if (pick === 1) {
    keys = keys.slice().reverse();
  } else if (pick === 2) {
    const anyBang = keys.some(k => k.startsWith('!'));
    keys = anyBang ? keys.map(k => k.replace(/^!+/, '')) : keys.map(k => '!' + k);
  } else if (pick === 3) {
    keys = keys.slice(0, -1);
  } else {
    keys = keys.concat(['adv']);
  }
  if (r.random() < 0.2) {
    head = head.split('#')[0] + r.choice(['', '#2', '#8']);
  }
  return head + '{' + keys.join(',') + '}';
}

function ways(h, method) {
  const out = [];
  for (const s of h.ics.values()) {
    if (s.method === method) {
      for (const w of s.ways) out.push(w);
    }
  }
  return out;
}

function calleePool(h) {
  const names = new Set(['intrinsic:Math.sqrt', 'intrinsic:Math.max', 'intrinsic:Math.floor']);
  for (const w of ways(h, true)) names.add(w[2]);
  for (const v of h.callee.values()) names.add(v);
  for (const w of ways(h, false)) {
    if (w[0] === 'accessor') {
      for (let i = 2; i < w.length; i++) names.add(w[i]);
    }
  }
  names.delete('*other*');
  return Array.from(names).filter(n => !n.startsWith('native:')).sort();
}

function pReceivers(h, r) {
  const pool = ways(h, false).concat(ways(h, true)).map(w => w[1]);
  if (pool.length === 0) return;
  for (const s of h.ics.values()) {
    for (const w of s.ways) {
      if (r.random() < 0.5) {
        w[1] = r.random() < 0.3 ? r.choice(pool) : reshape(w[1], r);
      }
    }
  }
}

function pForms(h, r) {
  const pool = calleePool(h);
  const poolOther = pool.concat(['*other*']);
  const prop = {
    own: ['proto', 'absent', 'accessor', 'store'],
    store: ['own', 'proto'],
    proto: ['own', 'absent', 'accessor'],
    absent: ['own', 'proto', 'accessor'],
    accessor: ['own', 'proto', 'absent'],
    array: ['own', 'absent'],
  };
  for (const s of h.ics.values()) {
    for (let i = 0; i < s.ways.length; i++) {
      if (r.random() >= 0.4) continue;
      const w = s.ways[i];
      if (s.method) {
        s.ways[i] = [r.choice(['direct', 'slot']), w[1], r.choice(poolOther)];
        continue;
      }
      const choices = prop[w[0]];
      if (!choices) continue;
      const form = r.choice(choices);
      let extra = [];
      if (form === 'proto') extra = [String(r.randint(1, 3)), String(r.randint(0, 3))];
      else if (form === 'accessor') extra = [r.choice(poolOther), r.choice(poolOther)];
      s.ways[i] = [form, w[1], ...extra];
    }
  }
}

function pPlaces(h, r) {
  for (const w of ways(h, false)) {
    if (w[0] === 'proto' && w.length === 4 && r.random() < 0.7) {
      w[2] = String(Math.max(1, parseInt(w[2], 10) + r.choice([-1, 1, 2])));
      const slot = parseInt(w[3], 10);
      w[3] = String(r.choice([0, slot + 1, Math.max(0, slot - 1)]));
    }
  }
}

function pReprs(h, r) {
  const d = h.held.doubles;
  const o = h.held.objects;
  const all = Array.from(new Set([...d.keys(), ...o.keys()])).sort();
  for (const name of all) {
    if (r.random() < 0.6) {
      const dv = d.get(name) || [];
      const ov = o.get(name) || [];
      d.set(name, ov);
      o.set(name, dv);
    }
  }
  for (const w of ways(h, false)) {
    const parts = keysOf(w[1]);
    if (parts && parts[1].length > 0 && !d.has(w[1]) && r.random() < 0.3) {
      d.set(w[1], parts[1].map((_, idx) => idx));
    }
  }
  h.other.mixed = [];
  h.other.nonobject = [];
}

function pUnseen(h, r) {
  const idxs = [...h.last.keys(), ...h.callee.keys()];
  for (const i of idxs) {
    if (r.random() < 0.5) {
      h.last.delete(i);
      h.callee.delete(i);
      h.slow.delete(i + 1);
      h.slow.delete(i - 5);
    }
  }
}

function pMonomorphic(h, r) {
  for (const s of h.ics.values()) {
    if (s.ways.length > 0) s.ways = [r.choice(s.ways)];
    s.flags = [];
    s.other = 0;
  }
}

function pCallees(h, r) {
  const pool = calleePool(h);
  for (const w of ways(h, true)) {
    if (r.random() < 0.6) w[2] = r.choice(pool);
  }
  for (const i of Array.from(h.callee.keys())) {
    h.callee.set(i, r.choice(pool));
    h.slow.set(i + 1, 1);
    h.slow.delete(i - 6);
  }
}

function pArms(h, r) {
  for (const i of h.armed()) {
    if (r.random() < 0.3) {
      if (h.slow.has(i)) {
        h.slow.delete(i);
      } else {
        let best = 1;
        for (let j = i - 7; j < i + 8; j++) {
          const v = h.slow.get(j) || 1;
          if (v > best) best = v;
        }
        h.slow.set(i, best);
      }
    }
  }
}

function pBorrowed(h, r) {
  for (const method of [false, true]) {
    const idx = [];
    for (const [i, s] of h.ics.entries()) {
      if (s.method === method) idx.push(i);
    }
    if (idx.length === 0) continue;
    for (const i of idx) {
      if (r.random() < 0.3) {
        const j = r.choice(idx);
        const tmp = h.ics.get(i);
        h.ics.set(i, h.ics.get(j));
        h.ics.set(j, tmp);
      }
    }
  }
  if (h.slow.size > 0) {
    const keys = Array.from(h.slow.keys()).sort((a, b) => a - b);
    const lo = r.choice(keys);
    const moved = new Map();
    for (const [i, c] of h.slow.entries()) {
      moved.set(i >= lo && i < lo + 400 ? i + 8 : i, c);
    }
    h.slow = moved;
  }
}

function pLifetimes(h, r) {
  const armedList = h.armed().sort((a, b) => a - b);
  const armedSet = new Set(armedList);
  for (const i of armedList) {
    if (!armedSet.has(i + 1) || r.random() >= 0.6) continue;
    const made = h.slow.get(i) || 0;
    if (made && (h.slow.get(i + 1) || 0) * 5 >= made * 4) {
      h.slow.delete(i + 1);
    } else {
      const m = Math.max(made, 256);
      h.slow.set(i, m);
      h.slow.set(i + 1, m);
    }
  }
}

function pHot(h, r) {
  if (!h.slowsites) return;
  for (const i of h.functions) {
    h.arm(i);
    h.slow.set(i, Math.max(h.slow.get(i) || 0, 64));
    if (r.random() < 0.3) {
      h.arm(i + 1);
      h.slow.set(i + 1, Math.max(h.slow.get(i + 1) || 0, 1 << 20));
    }
  }
}

function pCold(h, _r) {
  if (!h.slowsites) return;
  for (const i of h.functions) {
    for (const j of [i, i + 1]) {
      h.arm(j);
      h.slow.delete(j);
    }
  }
}

export const PROFILES = [
  [pReceivers, pForms, pPlaces, pReprs, pUnseen, pHot],
  [pMonomorphic, pCallees, pLifetimes, pHot],
  [pArms, pBorrowed, pHot],
  [pCold],
];

export function parseSlowSiteFunctions(ilText) {
  const fns = [];
  const re = /^func \S+\(.* counts #(\d+) \{$/gm;
  let m;
  while ((m = re.exec(ilText)) !== null) {
    fns.push(parseInt(m[1], 10));
  }
  return fns;
}
