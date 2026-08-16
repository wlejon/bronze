// The well-known string/regexp protocol, from the STRING side: each of
// `String.prototype`'s six pattern-taking members looks its argument up by a
// well-known symbol and hands the whole algorithm to what it finds.
//
// Expectations derived from 22.1.3.13 step 2 (match), 22.1.3.14 step 2
// (matchAll — whose IsRegExp and `g` check run BEFORE the dispatch), 22.1.3.19
// step 2 (replace), 22.1.3.20 step 2 (replaceAll, the same `g` check),
// 22.1.3.22 step 2 (search) and 22.1.3.23 step 2 (split), over 7.2.8 IsRegExp
// and 7.3.11 GetMethod. `RegExp.prototype`'s side of the same protocol is
// pinned by regexp_symbol_methods.

// The five keys are well-known symbols, each with 20.4.2's description.
console.log(typeof Symbol.match, typeof Symbol.matchAll, typeof Symbol.replace,
            typeof Symbol.search, typeof Symbol.split);
console.log(String(Symbol.replace), Symbol.split.toString());
console.log(Symbol.match === Symbol.match, Symbol.match === Symbol.matchAll);

// The method is called with the string, and its `this` is the PATTERN
// argument — not the string it was given.
const pattern = {
  tag: "P",
  [Symbol.match](s) { return this.tag + "|match|" + s; },
  [Symbol.search](s) { return s.length; },
  [Symbol.split](s, limit) { return [this.tag, s, String(limit)]; },
};
console.log("abc".match(pattern));
console.log("abcd".search(pattern));
// 22.1.3.23 step 2.b passes the LIMIT along, and passes it even when the call
// site omitted it.
console.log("xy".split(pattern, 3).join(","));
console.log("xy".split(pattern).join(","));

// `[Symbol.replace]` takes two arguments, and the second reaches it UNCONVERTED
// — 22.1.3.19 leaves ToString to 22.2.6.11 step 6, so a method of one's own
// sees whatever the call site wrote.
const replacer = {
  [Symbol.replace](s, value) { return "<" + s + "/" + typeof value + ":" + value + ">"; },
};
console.log("hi".replace(replacer, "Z"));
console.log("hi".replace(replacer, 7));

// `replaceAll` dispatches to the SAME key, so one method serves both members.
let seen = "";
const counter = {
  [Symbol.replace](s, value) { seen = seen + value; return s.toUpperCase(); },
};
console.log("ab".replaceAll(counter, "q"), "ab".replace(counter, "r"), seen);

const all = { [Symbol.matchAll](s) { return [s, s.length]; } };
console.log("mn".matchAll(all).join("-"));

// The lookup is an ordinary one, so a class that defines a hook on its
// PROTOTYPE is dispatched to exactly as an own property is.
class Censor {
  constructor(word) { this.word = word; }
  [Symbol.replace](s, value) { return s.split(this.word).join(value); }
  [Symbol.split](s) { return s.split(this.word); }
}
const censor = new Censor("--");
console.log("a--b--c".replace(censor, "+"));
console.log("a--b".split(censor).join("|"));

// 22.1.3.14 step 2.b / 22.1.3.20 step 2.b: an argument IsRegExp calls a regular
// expression must match globally, and the check runs BEFORE the dispatch — the
// method below is never entered.
let called = 0;
const nonGlobal = {
  [Symbol.match]: true,
  flags: "iy",
  [Symbol.matchAll](s) { called = called + 1; return "never"; },
  [Symbol.replace](s, value) { called = called + 1; return "reached"; },
};
try { "s".matchAll(nonGlobal); } catch (e) { console.log(e.name + ": " + e.message); }
try { "s".replaceAll(nonGlobal, "x"); } catch (e) { console.log(e.name + ": " + e.message); }
console.log(called);

// `replace` has no such step — replacing the FIRST match is something any
// pattern can do — so the very same argument reaches its method.
console.log("s".replace(nonGlobal, "x"));
console.log(called);

// A RegExp is regexp-like without any property read at all, so the same two
// members refuse a non-global one.
try { "s".matchAll(/s/); } catch (e) { console.log(e.name + ": " + e.message); }
try { "s".replaceAll(/s/, "x"); } catch (e) { console.log(e.name + ": " + e.message); }

// With `g` present the check passes and the dispatch happens.
const globalish = {
  [Symbol.match]: true,
  flags: "gi",
  [Symbol.matchAll](s) { return "all:" + s; },
  [Symbol.replace](s, value) { return "one:" + s + value; },
};
console.log("s".matchAll(globalish));
console.log("s".replaceAll(globalish, "!"));

// 7.2.8 step 2: `[Symbol.match]` set to a FALSY value opts an object OUT of
// being regexp-like, so no `flags` is read and no `g` is required.
const optedOut = { [Symbol.match]: false, [Symbol.matchAll](s) { return "out:" + s; } };
console.log("s".matchAll(optedOut));

// 7.3.11 step 4: a hook that is present and not callable is a TypeError, where
// an absent one falls through to the ordinary algorithm — an object with no
// hook is still just its ToString.
try { "a".split({ [Symbol.split]: 5 }); } catch (e) { console.log(e.name + ": " + e.message); }
console.log("a[object Object]b".replace({}, "-"));

// A hook found on a plain object does not change what the same string does with
// an ordinary pattern: the dispatch is per ARGUMENT, not a mode.
console.log("a1b22c".replace(/\d+/g, "#"), "a,b".split(",").join("|"), "hello".search(/l/));
