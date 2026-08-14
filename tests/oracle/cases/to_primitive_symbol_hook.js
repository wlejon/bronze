// ToPrimitive step 2: the `Symbol.toPrimitive` hook (ECMA-262 7.1.1,
// 20.4.2.1).
//
// `Symbol.toPrimitive` is a real well-known symbol, so an object can carry the
// method and 7.1.1 step 2 finds it — and when it does, the hook is the WHOLE
// answer: it is called with the hint as its one argument ("string", "number",
// or "default"), its primitive result is used directly, and the
// `valueOf`/`toString` search of step 3 never runs.
//
// The four lines pin the three hints: `String()` and a template substitution
// pass "string" (7.1.17 ToString), while `+` in both its concatenation and
// addition readings passes "default" (13.15.3 asks ToPrimitive for NO hint,
// which 7.1.1 step 2.b turns into "default") — which is why `'' + money` is
// "10" and not "ten".
const money = {
    [Symbol.toPrimitive](hint) {
        return hint === 'string' ? 'ten' : 10;
    },
};

console.log(String(money));
console.log(`${money}`);
console.log('' + money);
console.log(money + 1);
