// A GUARD PLACED AT THE END OF THE BLOCK THAT DEFINES ITS VALUE.
//
// `Quaternion.setFromEuler` is the shape: a defaulted parameter, then a header
// that computes six values through `Math.cos`/`Math.sin`, then a `switch` whose
// arms are the only places those six are ever read. No arm's block defines
// them and the header uses none of them, so the guard has nowhere to sit but at
// the END of the header — and the header is not the function's entry block,
// because the defaulted parameter put a branch in front of it.
//
// What that has to keep true is what this case checks by running:
//
//   * the fast copy computes what the slow copy would have;
//   * a value that is not a number sends control to the slow copy ONCE, at the
//     guard, with everything computed so far carried across — the six calls
//     have already happened and must not happen again;
//   * `is.number` reads bits, so no `valueOf` and no `toString` may run because
//     of a guard. The object below logs before it throws, and the log is how
//     many times ToPrimitive really ran;
//   * a replaced `Math.cos` that starts returning a string mid-header fails the
//     guard and the slow copy finishes the call — and the NEXT call, with the
//     replacement behaving again, takes the fast copy as before.

const log = [];

// The pair the header calls six times, in the position `Math.cos` and
// `Math.sin` hold in `setFromEuler`. Reached through an object so that one of
// them can be replaced while the program runs.
const ops = {
  f(t) { log.push('f'); return t + 1; },
  g(t) { log.push('g'); return t + 2; },
};

function combine(v, order, update = true) {
  const x = v.x, y = v.y, z = v.z, w = v.w;

  const f = ops.f;
  const g = ops.g;

  const c1 = f(x);
  const c2 = f(y);
  const c3 = f(z);

  const s1 = g(x);
  const s2 = g(y);
  const s3 = g(z);

  let a, b;

  switch (order) {
    case 'XYZ':
      a = s1 * c2 + c3 * 2;
      b = c1 + s2 * s3;
      break;

    case 'YXZ':
      a = s1 + c2 * c3;
      b = c1 * s2 - s3;
      break;

    default:
      a = c1 + c2 + c3;
      b = s1 + s2 + w;
  }

  // Logged rather than formatted into the result: a `+` with a string LITERAL
  // is a static proof that the guard would fail, so the pass drops the whole
  // candidate component a formatted return would join — and this case would
  // then be about one guard instead of the header's whole chain.
  log.push(a);
  log.push(b);

  return update ? a + b : a - b;
}

function show(label, run) {
  log.length = 0;
  let out;
  try {
    out = String(run());
  } catch (e) {
    out = 'throw ' + e.name + ': ' + e.message;
  }
  console.log(label + ' -> ' + out);
  console.log('  log ' + log.join(','));
}

const nums = { x: 1, y: 2, z: 3, w: 0 };

// Every value a number: the fast copy runs the whole function.
show('XYZ', () => combine(nums, 'XYZ'));
show('YXZ', () => combine(nums, 'YXZ'));
show('ZZZ', () => combine(nums, 'ZZZ'));

// The defaulted parameter passed explicitly, which is the other edge into the
// block the guard sits in.
show('XYZ no-update', () => combine(nums, 'XYZ', false));

// A string reaching the header: `f` and `g` concatenate it, the guard on the
// result fails, and the slow copy does the arithmetic the source asked for.
// The concatenation has to be exact, digit for digit.
const withStr = { x: 'S', y: 2, z: 3, w: 0 };
show('ZZZ string', () => combine(withStr, 'ZZZ'));
show('XYZ string', () => combine(withStr, 'XYZ'));

// An object whose `valueOf` logs and then throws. It is read in the header and
// used only in the default arm, so it is guarded with the rest — and the guard
// must not be what calls `valueOf`: exactly one `v` in the log, and it appears
// after all six calls, where the slow copy's `+` reaches it.
const boom = {
  valueOf() {
    log.push('v');
    throw new TypeError('boom');
  },
};
show('ZZZ throwing valueOf', () => combine({ x: 1, y: 2, z: 3, w: boom }, 'ZZZ'));

// `Math.cos` replaced, returning a string on its third call: the guard fails
// halfway through the header's six calls and the slow copy finishes.
let n = 0;
ops.f = function (t) {
  log.push('f');
  n = n + 1;
  return n === 3 ? 'X' + t : t + 1;
};
show('ZZZ replaced f', () => combine(nums, 'ZZZ'));

// The replacement is past its third call, so it is a number function again and
// the fast copy is taken again. Leaving the fast copy is per entry, not for
// good.
show('ZZZ after replacement', () => combine(nums, 'ZZZ'));
