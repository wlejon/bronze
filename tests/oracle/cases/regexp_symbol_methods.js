// The well-known string/regexp protocol, from the REGEXP side:
// `RegExp.prototype`'s symbol-keyed members are real function objects a program
// can reach and call — 22.2.6.8 [@@match], 22.2.6.9 [@@matchAll], 22.2.6.11
// [@@replace], 22.2.6.12 [@@search] and 22.2.6.14 [@@split].
//
// They are the algorithms, and `String.prototype`'s pattern members are triage
// in front of them (symbol_pattern_dispatch pins that side), so every pair
// below is one implementation asked twice.

console.log(typeof /x/[Symbol.match], typeof /x/[Symbol.matchAll],
            typeof /x/[Symbol.replace], typeof /x/[Symbol.search],
            typeof /x/[Symbol.split]);

// 22.2.6 puts each on the PROTOTYPE, so one function object serves every
// RegExp: an identity, not a twin per receiver.
console.log(/a/[Symbol.replace] === /b/[Symbol.replace]);
console.log(/a/[Symbol.replace] === /a/[Symbol.split]);

// Called explicitly, each answers what the string member does.
console.log(/x/[Symbol.replace]("axb", "y"), "axb".replace(/x/, "y"));
console.log(/\d/g[Symbol.replace]("a1b2", "#"), "a1b2".replace(/\d/g, "#"));
console.log(/[,;]/[Symbol.split]("a,b;c").join("|"), "a,b;c".split(/[,;]/).join("|"));
console.log(/(\d)/[Symbol.split]("a1b", 5).join("|"));
console.log(/l/[Symbol.search]("hello"), /z/[Symbol.search]("hello"));
console.log(/o/g[Symbol.match]("foo boo").join("|"));
console.log(/z/g[Symbol.match]("abc"));

// The `$` substitutions and the replacer-function protocol are the same ones,
// because 22.2.6.11 step 14 is the GetSubstitution the string member runs.
console.log(/(\w)(\d)/[Symbol.replace]("a1", "$2$1"));
console.log(/a/g[Symbol.replace]("aXa", function (m, off) { return off; }));

// 22.2.6.12 SAVES and restores `lastIndex`; 22.2.6.11 with a `g` pattern leaves
// it at zero.
const cursor = /l/g;
cursor.lastIndex = 3;
console.log(cursor[Symbol.search]("hello"), cursor.lastIndex);
const every = /a/g;
every.lastIndex = 1;
console.log(every[Symbol.replace]("aa", "b"), every.lastIndex);

// 22.2.6.9 steps 4-6 match with a CLONE, so walking the iterator cannot move
// the cursor of the RegExp it was asked for.
const source = /(\w)/g;
let walk = "";
for (const m of source[Symbol.matchAll]("pq")) { walk = walk + m[0] + m.index + ";"; }
console.log(walk, source.lastIndex);

// 22.2.9.2.1 step 8.a: a NON-GLOBAL matcher yields one match and is then done.
// Only reachable here — `String.prototype.matchAll` refuses a non-global
// argument before it ever gets this far.
let count = 0;
for (const m of /\w/[Symbol.matchAll]("pq")) { count = count + 1; }
console.log(count);

// 22.2.6's opening step in every one of the five: `this` must carry a
// [[RegExpMatcher]].
try { /x/[Symbol.replace].call("ax", "y"); } catch (e) { console.log(e.name + ": " + e.message); }
try { /x/[Symbol.split].call(null, "ax"); } catch (e) { console.log(e.name + ": " + e.message); }

// A pattern object that DELEGATES to RegExp.prototype's algorithm is what a
// program writes to wrap one, and the string members dispatch to it exactly as
// they do to a bare RegExp. (`class R extends RegExp` is refused by name: a
// subclass's instances would carry no [[RegExpMatcher]], so wrapping is the
// route a program has.)
const bracketed = {
  re: /[a-z]+/g,
  [Symbol.replace](s, value) { return "[" + this.re[Symbol.replace](s, value) + "]"; },
  [Symbol.split](s) {
    return this.re[Symbol.split](s).map(function (p) { return "<" + p + ">"; });
  },
  [Symbol.search](s) { return this.re[Symbol.search](s) * 10; },
};
console.log("a1b2".replace(bracketed, "-"));
console.log("1a2b3".split(bracketed).join(","));
console.log("12ab".search(bracketed));
