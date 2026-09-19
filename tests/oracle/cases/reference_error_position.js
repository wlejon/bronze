// ReferenceError positions: an unresolved-name read carries the identifier's
// own span, so the frame that raised `x is not defined` names the line the
// identifier sits on, not the line its function was declared on. Every
// function here spans several lines for exactly that reason.
//
// Lines are pinned everywhere. Columns are pinned only for a plain read that
// is not the first thing its statement evaluates: V8 gives the first bytecode
// of a statement the statement's own position (`return missingName` reports
// the `return`), and records no position at all for a callee's own load, so
// `missingFn()` reports its statement wherever it sits. Both are bytecode
// artifacts rather than source positions, so those sites pin the line alone.

function frames(s, withCol) {
  const out = [];
  for (const l of s.split('\n').slice(1)) {
    const m = l.match(/at (\S+) \(.*:(\d+):(\d+)\)$/);
    if (!m) break;
    out.push(withCol ? m[1] + '@' + m[2] + ':' + m[3] : m[1] + '@' + m[2]);
  }
  return out.join(',');
}

function report(f, withCol, depth) {
  try {
    f();
    console.log('no throw');
  } catch (e) {
    const fr = frames(e.stack, withCol).split(',').slice(0, depth || 1).join(',');
    console.log(e instanceof ReferenceError, e.stack.split('\n')[0], fr);
  }
}

function secondOperand() {
  const before = 1;
  return before + missingName;
}

function calledSecond() {
  const before = 1;
  return before + missingFn(before);
}

function updateStatement() {
  let n = 0;
  n += 1;
  missingCounter++;
  return n;
}

function compoundStatement() {
  let n = 0;
  missingAcc += n;
  return n;
}

function returnDirect() {
  const before = 1;
  return missingName;
}

function callDirect() {
  const before = 1;
  return missingFn(before);
}

function asArgument() {
  const before = 1;
  return Math.max(before, missingArg);
}

class K {
  m() {
    const before = 2;
    return before * missingInMethod;
  }
}

function inner() {
  const a = 1;
  const b = 2;
  return a + b + missingDeep;
}

function outer() {
  const x = 10;
  return x + inner();
}

report(secondOperand, true);
report(calledSecond, false);
report(updateStatement, true);
report(compoundStatement, true);
report(returnDirect, false);
report(callDirect, false);
report(asArgument, true);
report(() => new K().m(), true);
report(outer, true, 2);
console.log(typeof neverDefined);
