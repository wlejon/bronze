// The write audit's COMPUTED-WRITE triage, `types/field_audit.cpp`. A write
// `recv[key] = value` whose key the pass cannot enumerate is not a write to any
// particular name, so it can only be let past by proving the key numeric or the
// value numeric. Neither holds here: the key comes out of an array and the
// value is a string. So the site has to be REFUSED, and the only question left
// is how far the refusal reaches.
//
// It used to reach nowhere, because two guesses answered that question from how
// the RECEIVER IS SPELLED. `isArrayReceiverExpr` called a receiver an array
// because its identifier was one of ~65 names (`list`, `data`, `stack`,
// `nodes`, `cache`, ...) or ended in one of ~16 suffixes — an array can only be
// written under a numeric key, so the site was dismissed and only numeric-
// LOOKING names were put at risk. `isDictionaryOrMemberReceiver` called a
// receiver a dictionary whenever it was an identifier NOT spelled `o`, `obj`,
// `target` or `v` — every other name in the language, `holder` included — and
// dropped the site with no refusal at all. Either way `x` stayed number-clean,
// `V3.x` kept its primitive claim, and `sum()` read the stored string back
// through `unbox.f64 ..., raw`: `NaN` on both lines where the language says
// `hi1`.
//
// The same disease as `cases/field_audit_numeric_spelling.js` — a spelling
// spent as a proof — and it needed a different cure, because withdrawing the
// guesses with nothing in their place sends every unanalyzable computed write
// to `refuseAll`, and one of those refuses EVERY name in the program.
//
// What stands there now is a RECEIVER-SCOPED refusal. `settle()` has
// `typeOfExpr(c.receiver)` in hand: when that is an object this compilation
// WATCHED BEING MADE, the write reaches instances of that shape class and of
// nothing else, so the refusal is recorded against the class and its `extends`
// family rather than against the name everywhere. A receiver typed as a real
// array or typed array keeps the numeric-key answer it had. Only a receiver
// whose type is genuinely unknown still refuses the program.
//
// Both receivers below are `new V3()` bindings — watched being made — so both
// sites refuse `V3`'s fields and nothing else, and the reads answer with what
// the slots hold.

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

// Spelled `list`, which the array name list used to accept.
const list = new V3();
list[kk] = "hi";
list.y = 1;
console.log(list.sum());

// Spelled `holder`, which the dictionary rule used to accept — as it did every
// identifier but four.
const holder = new V3();
holder[kk] = "hi";
holder.y = 1;
console.log(holder.sum());
