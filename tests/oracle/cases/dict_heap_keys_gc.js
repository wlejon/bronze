// A dictionary's keys are heap strings that live only while their entries do.
// Each case here deletes keys while the runtime is still walking a list of them
// (a getter, a toJSON, a for-in body, a proxy trap runs in the middle) and then
// allocates enough for collections to run, so a key the walk had not kept would
// be freed — and its memory handed to the strings allocated after it. Run under
// BRONZE_GC_STRESS=1 too, where every allocation collects.

function churn(n) {
  // Fresh strings of the lengths the keys had, so freed key memory is reused —
  // as young strings, and as another dictionary's keys, which are old ones.
  const out = [];
  const sink = {};
  sink.t = 0;
  delete sink.t;
  for (let i = 0; i < n; i++) {
    const s = 'zz_' + (i % 97) + 'z'.repeat(i % 3);
    out.push(s);
    sink[s] = i;
  }
  return out.length;
}

function makeDict(prefix, n) {
  const d = {};
  d.tmp = 0;
  delete d.tmp;
  for (let i = 0; i < n; i++) d[prefix + '_' + i] = i;
  return d;
}

// Object.entries: the first getter deletes every later key.
{
  const d = makeDict('en', 12);
  Object.defineProperty(d, 'en_0', {
    get() {
      for (let i = 1; i < 12; i += 2) delete d['en_' + i];
      churn(400);
      return 'g';
    },
    enumerable: true,
    configurable: true,
  });
  console.log('entries', JSON.stringify(Object.entries(d)));
  console.log('after', Object.keys(d).join());
}

// Object.assign and object spread read through getters of the source too.
{
  const d = makeDict('as', 10);
  Object.defineProperty(d, 'as_1', {
    get() {
      for (let i = 2; i < 10; i += 3) delete d['as_' + i];
      churn(300);
      return 'g';
    },
    enumerable: true,
    configurable: true,
  });
  console.log('assign', JSON.stringify(Object.assign({}, d)));
  const d2 = makeDict('sp', 10);
  Object.defineProperty(d2, 'sp_0', {
    get() {
      for (let i = 5; i < 10; i++) delete d2['sp_' + i];
      churn(300);
      return 'g';
    },
    enumerable: true,
    configurable: true,
  });
  console.log('spread', JSON.stringify({ ...d2 }));
}

// JSON.stringify: a toJSON on a value deletes the keys after it.
{
  const d = makeDict('js', 8);
  d.js_2 = {
    toJSON() {
      for (let i = 3; i < 8; i++) delete d['js_' + i];
      churn(300);
      return 'J';
    },
  };
  console.log('json', JSON.stringify(d));
}

// for-in: the body deletes the keys it has already visited and churns. The
// remaining ones must still be handed out with their own names.
{
  const d = makeDict('fi', 30);
  const seen = [];
  for (const k in d) {
    seen.push(k + ':' + d[k]);
    delete d[k];
    churn(50);
  }
  console.log('forin', seen.length, seen[0], seen[29], Object.keys(d).length);
}

// The keys a program took out of a dictionary outlive their entries.
{
  const d = makeDict('kp', 20);
  const ks = Object.keys(d);
  const es = Object.entries(d);
  for (const k of ks) delete d[k];
  churn(500);
  console.log('kept', ks.length, ks[0], ks[19], es[7][0], es[7][1], ks.join('').length);
}

// A proxy over a dictionary: the traps run user code between the list and
// each key's descriptor.
{
  const d = makeDict('px', 10);
  const p = new Proxy(d, {
    getOwnPropertyDescriptor(t, k) {
      if (k === 'px_0') {
        for (let i = 1; i < 10; i += 2) delete t['px_' + i];
        churn(300);
      }
      return Reflect.getOwnPropertyDescriptor(t, k);
    },
  });
  console.log('proxy', Object.keys(p).join());
}

// getOwnPropertyNames / getOwnPropertyDescriptors over a dictionary whose
// values are churned between calls; and a dictionary re-filled with the keys
// it just lost, which must hash and compare as the same names.
{
  const d = makeDict('rf', 50);
  for (let r = 0; r < 4; r++) {
    for (let i = 0; i < 50; i++) delete d['rf_' + i];
    churn(200);
    for (let i = 0; i < 50; i++) d['rf_' + i] = i * r;
  }
  const names = Object.getOwnPropertyNames(d);
  const descs = Object.getOwnPropertyDescriptors(d);
  console.log('refill', names.length, names[49], descs.rf_49.value, d.rf_10, 'rf_10' in d);
}

// Many short-lived dictionary objects (each freed with its keys and table)
// interleaved with a long-lived one that keeps reading its own keys.
{
  const keep = makeDict('lv', 64);
  let sum = 0;
  for (let r = 0; r < 300; r++) {
    const tmp = makeDict('t' + r, 8);
    delete tmp['t' + r + '_3'];
    sum += Object.keys(tmp).length + keep['lv_' + (r % 64)];
  }
  console.log('churned', sum, Object.keys(keep).length, keep.lv_63);
}
