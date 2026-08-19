// 13.2.8.4 GetTemplateObject caches the template object PER CALL SITE, for the
// life of the program: every evaluation of one tagged template hands its tag
// the same object. That identity is the whole mechanism behind the memoizing
// tag — a WeakMap keyed on the strings array, which is how styled-components
// and graphql-tag avoid re-parsing — and without it every call missed the
// cache and re-did the work.
function tag(strings) { return strings; }

function site() { return tag`a${1}b`; }
console.log(site() === site());

function otherSite() { return tag`a${1}b`; }
console.log(site() === otherSite());

// The object is frozen, and so is `raw` (13.2.8.4 steps 12-13 use
// SetIntegrityLevel frozen on both).
const t = site();
console.log(Object.isFrozen(t), Object.isFrozen(t.raw));
console.log(t.length, t[0], t[1], t.raw.length);

// `raw` is the text before escape processing; `cooked` is after.
function rawTag(strings) { return strings[0] + "|" + strings.raw[0]; }
console.log(rawTag`x\ny`);

// One site inside a loop is still one site.
const seen = [];
for (let i = 0; i < 3; i++) seen.push(tag`loop`);
console.log(seen[0] === seen[1], seen[1] === seen[2]);

// A site inside a function called with different arguments is one site too:
// the SUBSTITUTIONS vary, the strings object does not.
function withArg(v) { return tag`p${v}q`; }
console.log(withArg(1) === withArg(2));

// The memoizing idiom, end to end.
const memo = new WeakMap();
let computed = 0;
function counting(strings) {
  if (!memo.has(strings)) {
    computed++;
    memo.set(strings, strings.join("*"));
  }
  return memo.get(strings);
}
function callA() { return counting`one${0}two`; }
function callB() { return counting`three`; }
callA(); callA(); callB(); callA(); callB();
console.log(computed, callA(), callB());
