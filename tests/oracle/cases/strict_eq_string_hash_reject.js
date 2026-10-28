// Dynamic === between two strings may answer false from lengths or cached
// hashes alone; these cases are the ones where that shortcut could lie:
// equal content built different ways (rope/builder growth, slices, two-byte
// storage, strings that were hashed as property keys first) and unequal
// content that collides on length.
function eq(a, b) { return a === b; }
function ne(a, b) { return a !== b; }
const out = [];
function rec(label, v) { out.push(label + ":" + v); }

let built = "";
for (let i = 0; i < 40; i++) built += String.fromCharCode(97 + (i % 26));
const lit = "abcdefghijklmnopqrstuvwxyzabcdefghijklmn";
rec("builder-vs-literal", eq(built, lit));
rec("builder-vs-literal-ne", ne(built, lit));

const keyed = {};
keyed[lit] = 1;
const copy = lit.slice(0);
const copy2 = (lit + "!").slice(0, lit.length);
keyed[copy2] = 2;
rec("hashed-vs-slice", eq(lit, copy2));
rec("hashed-vs-builder", eq(copy2, built));
for (const k in keyed) rec("key-eq", eq(k, built));

rec("same-length-diff", eq("abcd", "abce"));
rec("diff-length", eq("abc", "abcd"));
const h1 = "xyzw", h2 = "xyzv";
const m = {}; m[h1] = 1; m[h2] = 2;
rec("hashed-same-length-diff", eq(h1, h2));
rec("hashed-self", eq(h1, "xy" + "zw"));

const wide1 = "中文abc";
let wide2 = "中"; wide2 += "文"; wide2 += "abc";
rec("two-byte", eq(wide1, wide2));
const latinFromWide = ("中abc").slice(1);
rec("latin-from-wide", eq(latinFromWide, "abc"));
const mm = {}; mm[latinFromWide] = 1; mm["abc"] = 2;
rec("latin-from-wide-hashed", eq(latinFromWide, "abc"));
rec("empty", eq("", "".slice(0)));

rec("bigint-eq", eq(10n ** 30n, 10n ** 30n));
rec("bigint-ne", eq(10n ** 30n, 10n ** 30n + 1n));
rec("bigint-vs-string", eq(1n, "1"));
rec("string-vs-number", eq("1", 1));
rec("string-vs-object", eq("a", new String("a")));

// One site sees many pairs, equal and not, so its shortcut runs warm.
const pool = [];
for (let i = 0; i < 64; i++) {
  let s = "k";
  for (let j = 0; j < 1 + (i % 5); j++) s += (i * 7 + j) % 10;
  pool.push(s);
}
const tag = {};
for (let i = 0; i < pool.length; i += 3) tag[pool[i]] = i;
let hits = 0, total = 0;
for (let i = 0; i < pool.length; i++)
  for (let j = 0; j < pool.length; j++) {
    total++;
    if (eq(pool[i], pool[j])) hits++;
  }
rec("pool", hits + "/" + total);
let naive = 0;
for (let i = 0; i < pool.length; i++)
  for (let j = 0; j < pool.length; j++)
    if (pool[i].length === pool[j].length && [...pool[i]].every((c, k) => c === pool[j][k])) naive++;
rec("pool-naive", naive);
console.log(out.join("\n"));
