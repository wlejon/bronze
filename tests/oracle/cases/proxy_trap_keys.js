// What a trap is handed as the key: the result of ToPropertyKey (7.1.19), a
// string or a symbol, never the number the program wrote. `p[i] = v` with a
// numeric i reaches 10.5.9 [[Set]] as the string "1"; the same holds for
// `delete p[i]`, `i in p`, a read, Reflect.set / Reflect.deleteProperty and
// Object.defineProperty with a number. The element paths carry an index as a
// number, so a handler that tests `typeof key === "string"` (the way an
// indexed-access tracer does) saw reads and missed every write.

const seen = [];
const handler = {
  get(t, k) { seen.push("get " + typeof k + " " + String(k)); return t[k]; },
  set(t, k, v) { seen.push("set " + typeof k + " " + String(k)); t[k] = v; return true; },
  has(t, k) { seen.push("has " + typeof k + " " + String(k)); return k in t; },
  deleteProperty(t, k) { seen.push("delete " + typeof k + " " + String(k)); return delete t[k]; },
  defineProperty(t, k, d) { seen.push("define " + typeof k + " " + String(k)); return Reflect.defineProperty(t, k, d); },
  getOwnPropertyDescriptor(t, k) { seen.push("gopd " + typeof k + " " + String(k)); return Reflect.getOwnPropertyDescriptor(t, k); },
};

const i = 1;
const arr = new Proxy([0, 0, 0], handler);
arr[i] = 7;
arr[i + 1] = 8;
void arr[i];
void (i in arr);
delete arr[i];
Reflect.set(arr, 0, 5);
Reflect.deleteProperty(arr, 0);
Object.defineProperty(arr, 2, { value: 9, writable: true, enumerable: true, configurable: true });
Object.getOwnPropertyDescriptor(arr, 2);

const ta = new Proxy(new Float32Array(4), handler);
for (let j = 0; j < 2; j++) ta[j + 1] = ta[j] + 1;

const sym = Symbol("s");
const obj = new Proxy({}, handler);
obj[sym] = 1;
obj[2.5] = 1;
obj[-0] = 1;

for (const line of seen) console.log(line);
