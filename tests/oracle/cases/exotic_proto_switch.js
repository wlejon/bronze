// Trained code meets an exotic prototype (runtime/rt_exotic_link.cpp): the
// hot functions are trained on plain objects over plain prototypes (run.py's
// switch mode runs the real arm on a harvest of the training arm), so their
// compiled index and named reads, `in`, writes and for-in were specialized to
// chains that hold no index. The real arm hands them objects whose
// prototype is an array, a typed array or a String wrapper, then mutates
// that prototype under them.
const TRAINING = Math.random() < 0.5;

function readHot(o, n) {
  let s = '';
  for (let i = 0; i < n; i++) {
    const v = o[i % 3];
    s += (v === undefined ? '_' : v) + (i % 3 in o ? '+' : '-');
  }
  return s + ' len=' + o.length + ' k=' + o.k;
}

function writeHot(o, n) {
  for (let i = 0; i < n; i++) o[i % 2] = i;
  return [o[0], o[1], '[' + Object.keys(o).join(',') + ']'].join(' ');
}

function keysHot(o) {
  const ks = [];
  for (const k in o) ks.push(k);
  return ks.join(',');
}

function train() {
  let t = 0;
  for (let r = 0; r < 400; r++) {
    const base = { k: 'K', length: 3 };
    const o = Object.create(base);
    t += readHot(o, 9).length + writeHot(Object.create(base), 4).length + keysHot(o).length;
  }
  return t;
}

function real() {
  const out = [];
  const arr = [10, 20];
  arr.k = 'arrK';
  const a = Object.create(arr);
  out.push(readHot(a, 6), keysHot(a));
  arr.push(30);
  out.push(readHot(a, 6), keysHot(a));
  arr.length = 1;
  out.push(readHot(a, 6), keysHot(a));
  out.push(writeHot(a, 4), arr.join(','));

  const ta = new Int16Array([-1, -2, -3]);
  const t = Object.create(ta);
  // The view's `length` getter refuses a receiver that is not a view, so the
  // heir is given its own.
  const tl = Object.create(new Int16Array([5, 6]));
  Object.defineProperty(tl, 'length', { value: 2 });
  out.push(readHot(tl, 6));
  out.push(writeHot(t, 4), ta.join(','), keysHot(t));

  const s = Object.create(new String('xyz'));
  out.push(readHot(s, 6), keysHot(s));
  out.push(writeHot(s, 4));

  const ro = [1, 2];
  Object.defineProperty(ro, 1, { get() { return 'G' + this.k; }, set(v) { this.seen = v; } });
  const r = Object.create(ro);
  r.k = '!';
  out.push(readHot(r, 6), writeHot(r, 4), r.seen);
  return out.join('\n');
}

if (TRAINING) {
  console.error('TRAINING');
  console.log(train());
} else {
  console.log(train());
  console.log(real());
}
