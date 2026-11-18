// Selects whose one operand dies at the select and whose other is read
// again after it (brass x64_isel_select.cpp starts the cmov from the dying
// one), over objects that collections move (an allocation every iteration),
// and calls with double arguments (Math.atan2, Math.sin: no %al count set
// before a call). The run that did not train sends strings and a second
// shape through the same code, so speculated code leaves into its twin
// mid-loop with the selected references and doubles in its state.

const TRAINING = Math.random() < 0.5;
if (TRAINING) console.error('TRAINING');

class P {
  constructor(x, y) {
    this.x = x;
    this.y = y;
  }
}

function pick(a, b, k) {
  const c = new P(a.x + k, a.y - k);
  const s = k > 20 ? c : a;
  return s.x * 2 + s.y + a.x;
}

function angles(p, q, t) {
  const r = Math.atan2(p.y - q.y, p.x - q.x);
  const u = Math.sin(r * t) + Math.sin((1 - t) * r);
  const w = t > 0.5 ? u : r;
  return w + r * t + p.x;
}

function run(n, odd) {
  const keep = [];
  let sum = 0;
  let acc = 0;
  for (let i = 0; i < n; i++) {
    const a = odd && i === n - 7 ? { x: 'x' + i, y: 3 } : new P(i % 13, i % 5);
    const b = new P(i % 3, 1);
    sum += pick(a, b, i % 41);
    acc += angles(b, typeof a.x === 'number' ? a : b, (i % 10) / 10);
    if (i % 64 === 0) keep.push(new Array(32).fill(i));
    if (keep.length > 40) keep.length = 0;
  }
  return [sum, Math.round(acc * 1e6) / 1e6, keep.length];
}

for (let r = 0; r < 4; r++) console.log(run(30000, false).join(' '));
console.log(String(run(3000, !TRAINING)[0]).length > 0);
const last = run(200, false);
console.log(last.join(' '));
