// Long and wide strings as property and Map keys: equal content finds the
// same entry however the string was built, different content never does.
const base = 'abcdefghijklmnopqrstuvwxyz0123456789';
const obj = {}, map = new Map(), set = new Set();
for (let len = 0; len <= 40; len++) {
    const k = base.slice(0, len);
    obj[k] = len;
    map.set(k, len);
    set.add(k);
}
let ok = true;
for (let len = 0; len <= 40; len++) {
    const rebuilt = base.split('').slice(0, len).join('');
    const concat = base.slice(0, len >> 1) + base.slice(len >> 1, len);
    if (obj[rebuilt] !== Math.min(len, 36) && len <= 36) ok = false;
    if (map.get(concat) !== (len <= 36 ? len : 40)) ok = false;
    if (!set.has(rebuilt)) ok = false;
}
console.log(ok, Object.keys(obj).length, map.size, set.size);

// Keys differing in one unit at each position of a long key.
const long = 'k'.repeat(64);
const variants = {};
for (let i = 0; i < 64; i++) variants[long.slice(0, i) + 'K' + long.slice(i + 1)] = i;
variants[long] = -1;
let distinct = true;
for (let i = 0; i < 64; i++) if (variants[long.slice(0, i) + 'K' + long.slice(i + 1)] !== i) distinct = false;
console.log(distinct, variants[long], Object.keys(variants).length, variants[long + 'k'], variants[long.slice(1)]);

// A two-byte string whose content is all one-byte equals the one-byte key.
const narrow = 'plain-key-of-some-length';
const viaWide = ('你' + narrow).slice(1);
const store = { [narrow]: 'found' };
const m2 = new Map([[narrow, 'mapped']]);
console.log(store[viaWide], m2.get(viaWide), viaWide === narrow, viaWide in store);
const wideKeys = ['你好', '你好你好你好你好你', '😀', 'éèêëàâäåæ'];
const wideObj = {};
wideKeys.forEach((k, i) => { wideObj[k] = i; map.set(k, i); });
console.log(wideKeys.map((k) => wideObj[k.slice(0, 1) + k.slice(1)]).join(), wideKeys.map((k) => map.get(k + '')).join(),
    wideObj['你'], map.has('好你'));

// Many generated keys, deleted and re-added: a dictionary-mode object.
const cache = {};
for (let i = 0; i < 2000; i++) cache['style-' + i + '-' + (i * 7919 % 1000) + '-suffix'] = i;
let total = 0, missing = 0;
for (let i = 0; i < 2000; i++) {
    const k = ['style', i, i * 7919 % 1000, 'suffix'].join('-');
    if (k in cache) total += cache[k]; else missing++;
    if (i % 3 === 0) delete cache[k];
}
console.log(total, missing, Object.keys(cache).length, cache['style-1-919-suffix'], cache['style-0-0-suffix']);
