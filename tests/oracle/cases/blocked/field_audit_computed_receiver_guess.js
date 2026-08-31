// BLOCKED: bronze prints `NaN` where the language says `hi1`, on both lines.
//
// The write audit's COMPUTED-WRITE triage, `types/field_audit.cpp`. A write
// `recv[key] = value` whose key it cannot enumerate is not a write to any
// particular name, so it can only be let past by proving the key numeric or
// the value numeric. Neither holds here: the key comes out of an array and the
// value is a string. Two guesses let it through anyway, and both are guesses
// about how the RECEIVER IS SPELLED:
//
//   - `isArrayReceiverExpr` calls a receiver an array because its identifier
//     is one of ~65 names (`list`, `data`, `stack`, `nodes`, `cache`, ...) or
//     ends in one of ~16 suffixes. An array receiver can only be written under
//     a numeric key, so the site is dismissed and only numeric-LOOKING names
//     are put at risk. Here `list` is a `V3`, and the key is `x`.
//
//   - `isDictionaryOrMemberReceiver` calls a receiver a dictionary — a bag
//     whose keys are data, not fields anyone claims a layout for — whenever it
//     is an identifier NOT spelled `o`, `obj`, `target` or `v`. That is every
//     other name in the language, `holder` included, and a site it dismisses
//     is dropped from the audit with no refusal recorded at all.
//
// So `x` stays number-clean, `V3.x` keeps its primitive claim, and `sum()`
// reads the stored string back through `unbox.f64 ..., raw`.
//
// This is the same disease as `cases/field_audit_numeric_spelling.js` — a
// spelling spent as a proof — but it is not the same fix. Withdrawing these two
// guesses without anything in their place sends every unanalyzable computed
// write in a real library to `refuseAll`, and a single one of those refuses
// EVERY name in the program: on three.js there are 88 such sites, so the audit
// would certify nothing at all rather than certifying less.
//
// What the sound version needs is a receiver-scoped refusal. The audit is
// name-keyed and receiver-blind today, which is why an unknown key forces a
// program-wide answer. `settle()` already has `typeOfExpr(c.receiver)` in hand:
// when that is an object of a known shape class, the write can only reach that
// class's instances, so the refusal belongs to that class rather than to the
// name everywhere — a per-(class, field) refusal `fieldValueCandidate` already
// has the shape to carry. Only a receiver whose type is genuinely unknown needs
// to refuse the program. Until that exists, the two spellings above are load
// bearing, and this case is the record of what they cost.

class V3 {
  constructor() {
    this.x = 0;
    this.y = 0;
  }
  sum() {
    let s = this.x + this.y;
    for (let i = 0; i < 2; i++) s = this.x + this.y;
    return s;
  }
}

// The key is spelled nowhere as a literal the pass can follow to this site, so
// the write is genuinely computed.
const kk = ["x"][0];

// Receiver spelled `list`: taken for an array, so the site is taken for an
// element write under a numeric key.
const list = new V3();
list[kk] = "hi";
list.y = 1;
console.log(list.sum());

// Receiver spelled `holder`: taken for a dictionary, so the site is dropped.
const holder = new V3();
holder[kk] = "hi";
holder.y = 1;
console.log(holder.sum());
