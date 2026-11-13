// Sites whose receivers are mostly ones an inline cache cannot hold
// (docs/type-feedback.md, "Harvest", the `other` row): each meets a plain
// object of one shape first, which fills its cache, and then Arrays, a
// dictionary-mode object, strings, a function and a typed array on nearly
// every call, which never do. A harvest of such a site reads as that one shape
// plus an `other` count, so a build on it keeps the site off a one-shape
// claim; and whatever the harvest says, every line must read as the ordinary
// [[Get]] / [[Set]] / call, including across the twin's exits.

function Listener(fn, context) {
  this.fn = fn;
  this.context = context;
}

// The eventemitter3 shape of it: one listener is the object itself, two or
// more are an Array of them.
function on(events, name, fn) {
  const l = new Listener(fn, null);
  if (!events[name]) events[name] = l;
  else if (!events[name].fn) events[name].push(l);
  else events[name] = [events[name], l];
}
function count(events, name) {
  const ls = events[name];
  if (!ls) return 0;
  if (ls.fn) return 1;
  return ls.length;
}

function readTag(o) { return o.tag; }
function writeTag(o, v) { o.tag = v; return o.tag; }
function callSize(o) { return o.size(); }

const dict = { tag: "d", size() { return 7; } };
for (let i = 0; i < 40; i++) dict["k" + i] = i;
for (let i = 0; i < 40; i++) delete dict["k" + i];
function Fn() {}
Fn.tag = "f";
Fn.size = function () { return 3; };
const arr = [1, 2, 3];
arr.size = function () { return this.length; };
const typed = new Float32Array(4);
const receivers = [arr, dict, "str", Fn, typed, [9]];

let total = 0;
let tags = "";
for (let round = 0; round < 3000; round++) {
  const events = {};
  for (let k = 0; k < 5; k++) on(events, "update", () => k);
  total += count(events, "update");
  const single = {};
  on(single, "x", () => 0);
  total += count(single, "x");

  // One plain object once, then the others every round.
  const plain = round === 0 ? { tag: "p", size() { return 1; } } : null;
  if (plain) {
    tags += readTag(plain) + writeTag(plain, "q");
    total += callSize(plain);
  }
  const r = receivers[round % receivers.length];
  const t = readTag(r);
  if (round < receivers.length) tags += String(t);
  if (typeof r === "object" && r !== null && !Array.isArray(r) && !ArrayBuffer.isView(r)) writeTag(r, "w" + (round % 3));
  if (typeof r !== "string" && typeof r.size === "function") total += callSize(r);
}
console.log(total, tags);
console.log(dict.tag, Fn.tag, arr.size(), readTag([]), readTag("s"), readTag(typed));
