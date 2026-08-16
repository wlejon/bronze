// `Date.prototype[Symbol.toPrimitive]` (ECMA-262 21.4.4.45) and the operators
// that go through it.
//
// This clause is the one place in the language where hint DEFAULT behaves as
// hint STRING, and everything below is a consequence of that single line:
// `date + 1` CONCATENATES where `date - date` SUBTRACTS, and `date == string`
// compares text where `date < date` compares instants.
//
// `toString` carries the local zone offset, so the concatenated forms are
// compared against `d.toString()` rather than pinned as bytes.

const d = new Date(Date.UTC(2020, 0, 1));
const e = new Date(Date.UTC(2020, 0, 2));

// ---- the hook itself --------------------------------------------------------
console.log(typeof d[Symbol.toPrimitive]);
console.log(d[Symbol.toPrimitive]("number"), d.getTime());
console.log(d[Symbol.toPrimitive]("string") === d.toString());
console.log(d[Symbol.toPrimitive]("default") === d.toString());
try {
  d[Symbol.toPrimitive]("other");
  console.log("no throw");
} catch (err) {
  console.log(err instanceof TypeError);
}
// It is an INHERITED member, so it is not an own key of the instance.
console.log(Object.getOwnPropertySymbols(d).length,
            Symbol.toPrimitive in d);

// ---- `+` asks with no hint, and lands in the string branch ------------------
console.log(typeof (d + 1));
console.log(d + 1 === d.toString() + "1");
console.log("" + d === d.toString());
console.log(`${d}` === d.toString());
console.log(String(d) === d.toString());
console.log(typeof (d + d), d + d === d.toString() + d.toString());

// ---- every other arithmetic operator asks for a number ----------------------
console.log(e - d);
console.log(typeof (e - d));
console.log(d * 2, d / 2, +d, -d);
console.log(Number(d), Math.trunc(d / 86400000));
console.log(d % 1000);

// ---- the relational operators (13.10.1) ------------------------------------
// 7.2.13 IsLessThan asks with hint NUMBER, so `<` compares INSTANTS and not the
// text `toString` would have produced.
console.log(d < e, e < d, d <= d, d >= d, d > e);
console.log(new Date(Date.UTC(2020, 1, 1)) < new Date(Date.UTC(2020, 10, 1)));
// And the split is directly visible on ONE pair: two distinct Dates naming the
// same instant are `<=` each other (a numeric comparison of equal instants) and
// NOT `==` each other (two objects, compared by identity, with no conversion at
// all). One pair, two operators, two different questions.
{
  const a = new Date(Date.UTC(2020, 0, 1));
  const b = new Date(Date.UTC(2020, 0, 1));
  console.log(a <= b, b <= a, a < b, a == b);
}

// ---- equality ---------------------------------------------------------------
// `==` against a primitive is 7.2.14 steps 11/12: ToPrimitive with hint
// DEFAULT, which for a Date is the STRING. So a Date equals its own toString
// text and does NOT equal its own time value.
console.log(d == d.toString());
console.log(d == d.getTime());
console.log(d === d, d === new Date(d.getTime()));
console.log(d == new Date(d.getTime()));
console.log(d.getTime() === new Date(d.getTime()).getTime());
// Two Dates compared with `==` are two objects: identity, no conversion.
console.log(new Date(0) == new Date(0));

// ---- an override wins, because the lookup is an ordinary property read ------
{
  const own = new Date(0);
  own[Symbol.toPrimitive] = function (hint) { return hint; };
  console.log(own + "", own * 1, `${own}`);
}
{
  const noHook = new Date(0);
  noHook[Symbol.toPrimitive] = null;
  // 7.1.1 step 2 treats null as absent, so OrdinaryToPrimitive runs — and its
  // DEFAULT order is valueOf first, which is a number. This is the one way to
  // see that a Date's default-is-string comes from the hook and not from
  // anywhere else.
  console.log(typeof (noHook + 1), noHook + 1);
}

// ---- what console.log prints ------------------------------------------------
// node's rendering: the ISO form, bare. It is the only form of a Date that is
// the same on every machine — `toString` would carry the local offset.
console.log(new Date(0));
console.log(new Date(Date.UTC(2020, 0, 1, 2, 3, 4, 5)));
console.log(new Date(NaN));
console.log([new Date(0), new Date(86400000)]);
console.log({ at: new Date(0) });
