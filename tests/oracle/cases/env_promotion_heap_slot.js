// Stage R3 and the collector. `held` is a captured slot that holds a STRING —
// a heap address — and the loop that writes it allocates a fresh one every
// iteration. The heap slot is the collector's only view of what the record
// holds, so a register shadowing it while a collection runs would hand back an
// address nothing kept alive; under BRONZE_GC_STRESS, which collects at every
// allocation, that is not a subtle failure.
//
// `tag` beside it is a number, and is the control: nothing about it can be a
// heap address, so whatever the rule decides for `held` it must not decide for
// the same reason here.
function make() {
  let held = "seed";
  let tag = 0;

  function churn(rounds) {
    for (let i = 0; i < rounds; i++) {
      tag = tag + 1;
      held = "v" + tag;
    }
    return held;
  }

  return { churn: churn, get: function () { return held + "/" + tag; } };
}

const m = make();
console.log(m.churn(200));
console.log(m.get());
