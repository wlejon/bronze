// The REACH boundary of the dead-zone analysis — stage E4's widening, and the
// one place in the compiler where a wrong static answer is SILENT.
//
// `dead_zone_reachability.js` beside this file pins stage E3's rule: the PREFIX
// of a statement list that runs no user code at all. Stage E4 widened that to a
// REACH — the scan now walks PAST a statement that neither BUILDS A FUNCTION
// nor MENTIONS a name still in its dead zone, so a setup loop or a `try` no
// longer stops it. That widening is what this file is about, because every
// other mechanism in the campaign fails LOUD and this one fails quiet: an
// elided check on a binding the analysis wrongly cleared answers `undefined`
// where the language says ReferenceError, and nothing anywhere says so.
//
// The argument the widening rests on has exactly two clauses, and every case
// below attacks one of them:
//
//   (1) Code in this statement list can only read a binding of this record by
//       NAMING it, and the mention scan is built on the same complete walk the
//       capture analysis uses (ast/query_walk.h `IdentVisitor`, whose base
//       declares one pure virtual per node kind, so no form can be silently
//       skipped). bronze has no `with`, and its direct `eval` runs with
//       INDIRECT semantics against the global environment and says so with a
//       warning — so there is no third way to name a binding.
//
//   (2) Any OTHER reader is a closure over this record, and a closure over
//       this record is created either by a function-or-class-building node
//       written inside this list — which stops the scan on sight — or by one
//       of this list's own hoisted `function` declarations, whose names are in
//       the stop set for the whole scan and can therefore never be mentioned
//       by a statement the scan walks past.
//
// Every case is written as a PAIR of claims: what must still throw, and — for
// the widening to be worth having — what must not. `tryIt` turns a throw into
// data so that the whole file runs to the end and the last line is as much of
// the oracle as the first.

function tryIt(fn) {
  try {
    return `ok:${fn()}`;
  } catch (e) {
    return e.name;
  }
}

// 1. A `function` DECLARATION nested inside a block statement in the prefix.
//    The scan's "builds a function" test does not fire on a FunctionDecl —
//    only on a function or class EXPRESSION — so what has to catch this is
//    clause (1) walking the declaration's BODY and finding the mention.
function nestedDeclInBlock() {
  const marks = [];
  {
    function grab() { return later; }
    marks.push(tryIt(grab));
  }
  let later = "nested";
  return `${marks[0]}/${later}`;
}

// 2. The same nested declaration, reaching the binding through a SECOND hop
//    instead of naming it: `grab` calls `peek`, and `peek` is a hoisted
//    declaration of the outer list. Clause (2)'s stop set is what catches it —
//    `grab`'s body mentions `peek`, and `peek` never leaves the set.
function nestedDeclTwoHops() {
  const marks = [];
  {
    function grab() { return peek(); }
    marks.push(tryIt(grab));
  }
  let hop = "two-hops";
  function peek() { return hop; }
  return `${marks[0]}/${hop}`;
}

// 3. A closure stored on an object and called from the prefix. The store is an
//    ordinary expression statement, so nothing about it is a declaration —
//    what refuses it is the FunctionExpr in its subtree.
function storedThenCalled() {
  const box = {};
  box.f = function () { return hidden; };
  const r = tryIt(box.f);
  let hidden = "stored";
  return `${r}/${hidden}`;
}

// 4. An object literal GETTER, which is a function the source never spells as
//    a call and the scan must still see.
function getterInPrefix() {
  const o = { get g() { return gLate; } };
  const r = tryIt(() => o.g);
  let gLate = "getter";
  return `${r}/${gLate}`;
}

// 5. A class EXPRESSION, which is method syntax wearing a different hat.
function classInPrefix() {
  const C = class {
    m() { return cLate; }
  };
  const r = tryIt(() => new C().m());
  let cLate = "class";
  return `${r}/${cLate}`;
}

// 6. `try`/`catch`, whose whole point is that control does NOT run the
//    statements between the throw and the handler. The scan walks the list in
//    source order and has to be right about a list some of which never ran.
function tryCatchOrder() {
  const marks = [];
  try {
    marks.push(tryIt(grabTC));
    throw new Error("stop");
  } catch (e) {
    marks.push("caught");
  }
  let tcLate = "try";
  function grabTC() { return tcLate; }
  return `${marks.join("|")}/${tcLate}`;
}

// 7. A LOOP whose first iteration reads what a statement below it initializes.
//    This is the shape the widening exists to walk past, with the one thing in
//    it that makes walking past wrong.
function loopReadsLater() {
  const marks = [];
  for (let i = 0; i < 2; i++) {
    marks.push(tryIt(loopPeek));
  }
  let loopVal = "loop";
  function loopPeek() { return loopVal; }
  return `${marks.join("|")}/${loopVal}`;
}

// 8. The same loop with nothing dangerous in it — the widening's PAYOFF, and
//    the half of the pair that must NOT throw. If this ever starts reporting a
//    ReferenceError the analysis has been narrowed back to stage E3's prefix.
function loopIsHarmless() {
  const xs = [];
  for (let i = 0; i < 3; i++) {
    xs.push(i * 2);
  }
  let total = xs.length;
  function readTotal() { return total; }
  return `${readTotal()}:${xs.join(",")}`;
}

// 9. A mention hidden in a COMPUTED member, read directly rather than through
//    any closure at all: the narrowest possible test of clause (1)'s walk.
function computedMention() {
  const map = { a: "mapped" };
  const got = tryIt(() => map[later]);
  let later = "a";
  return `${got}/${map[later]}`;
}

// 10. A parameter DEFAULT that builds a closure. The parameter list is
//     evaluated before the body's first statement, so a closure there is one
//     the scan can never see created — which is why the whole widening is
//     refused for a scope whose defaults build functions, rather than
//     modelled.
function paramDefaultClosure(mk = function () { return pdLate; }) {
  const r = tryIt(mk);
  let pdLate = "default";
  return `${r}/${pdLate}`;
}

// 11. A GENERATOR, whose body suspends: arbitrary user code runs between two
//     of its statements. Nothing outside can name its bindings, so the
//     suspension itself is harmless — what is not harmless is the same
//     mention as everywhere else, in a list the machine re-enters.
function generatorSuspend() {
  const marks = [];
  function* g() {
    marks.push(tryIt(peekG));
    yield "first";
    let gv = "gen";
    function peekG() { return gv; }
    yield gv;
  }
  const it = g();
  marks.push(String(it.next().value));
  marks.push(String(it.next().value));
  return marks.join("|");
}

// 12. A `switch`, the one statement list in the language control can enter in
//     the MIDDLE of. Every clause shares one block, so a `case` above a
//     declaration is a reachable dead zone with no statement between them.
function switchClauses(k) {
  switch (k) {
    case 1:
      return `saw:${String(sv)}`;
    case 2:
      let sv = "switched";
      return `set:${sv}`;
    default:
      return "none";
  }
}

// 13. Two statements that run user code and then a declaration BELOW them,
//     which is the plain widening with no hazard in it at all — the second
//     half of the pair for cases 6 and 7.
function harmlessPrefix() {
  const parts = [];
  parts.push("a");
  try {
    parts.push("b");
  } catch (e) {
    parts.push("never");
  }
  let tail = parts.length;
  function readTail() { return tail; }
  return `${readTail()}:${parts.join("")}`;
}

console.log(nestedDeclInBlock());
console.log(nestedDeclTwoHops());
console.log(storedThenCalled());
console.log(getterInPrefix());
console.log(classInPrefix());
console.log(tryCatchOrder());
console.log(loopReadsLater());
console.log(loopIsHarmless());
console.log(computedMention());
console.log(tryIt(paramDefaultClosure));
console.log(generatorSuspend());
console.log(tryIt(() => switchClauses(1)));
console.log(switchClauses(2));
console.log(harmlessPrefix());

// 14. And the same boundary at MODULE level, where the record is shared with
//     every module function and the scan is handed the whole merged body.
const modParts = [];
for (let i = 0; i < 2; i++) {
  modParts.push(tryIt(modPeek));
}
let modLate = "module";
function modPeek() { return modLate; }
console.log(`${modParts.join("|")}/${modLate}`);
