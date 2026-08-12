// A property WRITE whose receiver is a primitive, in both spellings.
//
// `o.k = v` and `o[i] = v` are one operation, and the two used to disagree
// about this: the named form threw a TypeError a `catch` could hold, and the
// computed form killed the process by name. `cases/string_index` pins the same
// agreement on the READ side, and for the same reason — which spelling the
// source used is not a fact about the program's meaning.
//
// The answer is the TypeError, and which TypeError comes from ECMA-262 rather
// than from a preference. 13.15.2 PutValue step 6.d throws when the reference
// is strict, and a module is always strict code (16.2.1.6), so every one of
// these is a throw and none of them is the sloppy silent discard. A discarded
// write is the shape the house rules forbid twice over: the program believes it
// stored something, and nothing says otherwise.
//
// What this pins:
//
// 1. A write to a property of a string, a number and a boolean is a TypeError,
//    whichever spelling asked for it. `e instanceof TypeError` and `e.name`
//    rather than the message, because the message is bronze's own text and only
//    the CLASS is what 13.15.2 fixes.
// 2. The primitive is unchanged afterwards, and reading the index it refused to
//    write still answers what 10.4.3.5 says it does. A refused write leaves no
//    trace, which is the other half of "the program believes it stored
//    something".
// 3. null and undefined are the same throw from one clause earlier — 7.3.4
//    ToObject, reached before any property is considered — so `null[0] = 1` and
//    `null.k = 1` agree with each other too.
const s = "abc";
try {
  s.k = 1;
  console.log("no throw");
} catch (e) {
  console.log("string named:", e instanceof TypeError, e.name);
}
try {
  s[0] = "x";
  console.log("no throw");
} catch (e) {
  console.log("string index:", e instanceof TypeError, e.name);
}
console.log(s, s[0]);

const n = 1;
try {
  n.k = 1;
  console.log("no throw");
} catch (e) {
  console.log("number named:", e instanceof TypeError, e.name);
}
try {
  n[0] = 1;
  console.log("no throw");
} catch (e) {
  console.log("number index:", e instanceof TypeError, e.name);
}

const b = true;
try {
  b[0] = 1;
  console.log("no throw");
} catch (e) {
  console.log("boolean index:", e instanceof TypeError, e.name);
}

const nothing = null;
try {
  nothing.k = 1;
  console.log("no throw");
} catch (e) {
  console.log("null named:", e instanceof TypeError, e.name);
}
try {
  nothing[0] = 1;
  console.log("no throw");
} catch (e) {
  console.log("null index:", e instanceof TypeError, e.name);
}
