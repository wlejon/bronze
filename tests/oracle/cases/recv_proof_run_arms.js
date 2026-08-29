// A RUN OF READS EMITTED AS TWO ARMS (src/codegen-llvm/llvm_run_arms.h), and
// every way the branch in front of it can go the other way.
//
// The shape: a run whose members are consecutive instructions is emitted as ONE
// test of the receiver proof over a fast arm that is nothing but loads and a
// slow arm that is the per-access cache ladder, joined at one block. Three
// things have to survive that and nothing here is subtle about which:
//
//   what the run READ. The fast arm holds it in registers and writes no root
//   slot at all, so an allocation after the join has to find it somewhere the
//   collector forwards — the join's own store, where anything can still reach
//   the slot, and the register everywhere else. `mix` reads its values again
//   AFTER a loop that allocates, which is where a wrong answer is a moved
//   object read through a stale word rather than a slow program.
//
//   what was live ACROSS the run. `before` is made in front of the run and read
//   behind it: the fast arm hands its register to the join and the slow arm,
//   whose ladders can run a getter and collect, reloads it out of its slot.
//
//   which arm ran. Both, in the same function, at the same sites: a dense array
//   takes the loads, and a plain object, a short array, a typed array, a string
//   and an `arguments` object take the ladder. A site that has seen both is a
//   site whose join has been reached from both sides.
//
// The last case is the sharpest: a run inside a GUARDED NUMERIC REGION whose
// fast copy bails part way through the guard chain. The bail block collects
// every value the run produced and hands them to the slow copy — off registers
// the join phi'd, from sixteen different guard blocks — so a value the arms
// forgot to hand forward is read there and nowhere else.

// The live-across value comes out of a CALL, and out of nothing else: a call is
// the last thing that can collect before the run, so the register holding its
// result is current where the run starts and the join has to hand it on. An
// object literal would not do — its own field store collects again, and the
// read after the run would go back to the slot and never touch the join at all.
function wrap(x) { return { n: x }; }

function mix(a, b, seed) {
  // Made before the run, read after it: live across both arms.
  const before = wrap(seed);
  const a0 = a[0];
  const a1 = a[1];
  const a2 = a[2];
  const a3 = a[3];
  const b0 = b[0];
  const b1 = b[1];
  const b2 = b[2];
  const b3 = b[3];
  // Read STRAIGHT off the join, with nothing allocating between the run and
  // here: the value that reaches this read is the register the join handed
  // over, on both arms, and on the slow arm a getter has just collected.
  const kept = before.n;
  const s = a0 * 1 + a1 * 2 + a2 * 3 + a3 * 4 + b0 * 5 + b1 * 6 + b2 * 7 + b3 * 8;
  // Allocation with everything the run read still wanted.
  const junk = [];
  for (let i = 0; i < 8; i++) junk.push({ i: i });
  return String(s) + '|' + String(kept) + '|' + String(junk.length) + '|' +
         String(a0) + ',' + String(b3);
}

// The same two runs with every result consumed by arithmetic alone: nothing
// reads a slot, so the fast arm writes none.
function dot(a, b) {
  const a0 = a[0];
  const a1 = a[1];
  const a2 = a[2];
  const a3 = a[3];
  const b0 = b[0];
  const b1 = b[1];
  const b2 = b[2];
  const b3 = b[3];
  return a0 * b0 + a1 * b1 + a2 * b2 + a3 * b3;
}

// `arguments` reaches the same sites as an object that is not an Array.
function viaArguments(a) {
  return mix(arguments, a, 'args');
}

function dense() { return [1, 2, 3, 4]; }
function other() { return [10, 20, 30, 40]; }

// 1. Both runs on the fast arm, twice: the second call is the one whose sites
//    have already cached everything they can cache.
console.log(mix(dense(), other(), 'one'));
console.log(mix(dense(), other(), 'two'));
console.log(String(dot(dense(), other())));

// 2. The FIRST run fast and the second slow, then the other way round, so each
//    of the two joins is reached from both of its predecessors.
console.log(mix(dense(), { 0: 10, 1: 20, 2: 30, 3: 40 }, 'objb'));
console.log(mix({ 0: 1, 1: 2, 2: 3, 3: 4 }, other(), 'obja'));
console.log(String(dot(dense(), { 0: 10, 1: 20, 2: 30, 3: 40 })));

// 3. An array shorter than the run's largest index: the one length test refuses
//    and every member takes the ladder, which answers `undefined` past the end.
console.log(mix([1, 2], other(), 'short'));
console.log(mix([], other(), 'empty'));

// 4. A HOLE. `delete` leaves `length` alone, so the length test still passes and
//    the fast arm's own correction is what turns the hole into `undefined`.
const holed = dense();
delete holed[2];
console.log(mix(holed, other(), 'holed'));

// 5. Frozen: still a dense Array, so still the fast arm.
console.log(mix(Object.freeze(dense()), other(), 'frozen'));

// 6. Receivers that are not Arrays at all, at the same sites.
console.log(mix(new Float64Array([1, 2, 3, 4]), other(), 'f64'));
console.log(mix('abcd', other(), 'string'));
console.log(viaArguments(other()));

// 7. A getter on every index of the first receiver, and each one ALLOCATES.
//    That is the slow arm's whole obligation: it collects between its own
//    members, so what it read before must be in a slot the collector forwards.
const getters = {};
let hits = 0;
for (let g = 0; g < 4; g++) {
  Object.defineProperty(getters, String(g), {
    get: function () {
      hits = hits + 1;
      const grow = [];
      for (let k = 0; k < 40; k++) grow.push({ k: k });
      return grow.length / 40;
    },
    configurable: true
  });
}
console.log(mix(getters, other(), 'getters'));
console.log(String(hits));
console.log(String(dot(getters, other())));
console.log(String(hits));

// 8. Indices reached through a PROTOTYPE rather than owned: no run's fast arm
//    can answer one, and the ladder's proto walk has to.
const protoIdx = { 2: 3, 3: 4 };
const inherited = Object.create(protoIdx);
inherited[0] = 1;
inherited[1] = 2;
console.log(mix(inherited, other(), 'proto'));

// 9. The guarded region's fast copy BAILING part way through its guard chain.
//    `a[2]` is a string, so the third numberness guard fails and the bail block
//    hands every value the run produced to the slow copy.
console.log(mix([1, 2, 'x', 4], other(), 'bail'));
console.log(mix(dense(), [10, 20, 'y', 40], 'bail2'));
console.log(String(dot([1, 2, 'x', 4], other())));

// 10. Back to two dense arrays, so the sites the lines above made polymorphic
//     are shown still taking the arm they were written for.
console.log(mix(dense(), other(), 'again'));
console.log(String(dot(dense(), other())));
