// The parts of `switch` that `switch.js` does not reach: how many times the
// discriminant runs, how far down the case list evaluation gets, what a
// `break` inside a switch inside a loop leaves, and what a switch with no
// matching case and no default does.
//
// Every answer is derived from ECMA-262 14.12.4 (CaseBlockEvaluation) and
// 14.12.10 (CaseClauseIsSelected):
//
// 1. 14.12.4 evaluates the switch's Expression ONCE, before any clause, so a
//    discriminant with a side effect fires one time however long the case
//    list is. `calls` ends at 1.
// 2. CaseClauseIsSelected evaluates each CaseClause's Expression in order and
//    stops at the first that IsStrictlyEqual. A `case` expression is an
//    arbitrary expression, so that is observable: `probe(3)` never runs, and
//    `probes` is "12" rather than "123".
// 3. A `break` with no label targets the innermost BREAKABLE statement, and a
//    switch is one (14.12), so it leaves the switch and the enclosing loop
//    keeps going. A `continue` is not breakable-relative: it looks for the
//    innermost ITERATION statement (14.9), which is the loop, so it skips the
//    rest of the loop body too.
// 4. No case selected and no default clause: the CaseBlock completes normally
//    with `empty`, so nothing in the body runs at all.
// 5. IsStrictlyEqual is `===`, which says NaN matches nothing (not even NaN)
//    and that +0 and -0 are the same value. `case NaN:` is therefore dead
//    code, and `switch (-0)` selects `case 0:`.
let calls = 0;
function disc() {
  calls = calls + 1;
  return 2;
}
let probes = "";
function probe(n) {
  probes = probes + n;
  return n;
}
switch (disc()) {
  case probe(1):
    console.log("one");
    break;
  case probe(2):
    console.log("two");
    break;
  case probe(3):
    console.log("three");
    break;
}
console.log(calls);
console.log(probes);

let seen = "";
for (let i = 0; i < 4; i++) {
  switch (i) {
    case 1:
      seen = seen + "a";
      break;
    case 2:
      continue;
    case 3:
      seen = seen + "c";
      break;
    default:
      seen = seen + "d";
  }
  seen = seen + ".";
}
console.log(seen);

function nomatch(v) {
  let r = "none";
  switch (v) {
    case 1:
      r = "one";
      break;
  }
  return r;
}
console.log(nomatch(1));
console.log(nomatch(9));

function nested(a, b) {
  let out = "";
  switch (a) {
    case 1:
      switch (b) {
        case 1:
          out = out + "x";
          break;
        default:
          out = out + "y";
      }
      out = out + "!";
      break;
    default:
      out = out + "z";
  }
  return out;
}
console.log(nested(1, 1));
console.log(nested(1, 2));
console.log(nested(2, 1));

// A block gives a case clause a scope of its own, which is how a declaration
// is written inside one. Two sibling clauses may each declare `t`.
function kind(v) {
  switch (typeof v) {
    case "number": {
      const t = "num:" + v;
      return t;
    }
    case "string": {
      const t = "str:" + v;
      return t;
    }
    default:
      return "other";
  }
}
console.log(kind(3));
console.log(kind("hi"));
console.log(kind(true));

function nanCase(v) {
  switch (v) {
    case NaN:
      return "nan";
    default:
      return "not-nan";
  }
}
console.log(nanCase(NaN));
console.log(nanCase(0 / 0));

function zeroCase(v) {
  switch (v) {
    case 0:
      return "zero";
    default:
      return "other";
  }
}
console.log(zeroCase(-0));
console.log(zeroCase(0));
