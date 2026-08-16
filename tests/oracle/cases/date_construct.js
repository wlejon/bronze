// The Date constructor, ECMA-262 21.4.2: the four argument forms, MakeFullYear's
// two-digit mapping, the field overflow that MakeDay/MakeTime normalise, and
// TimeClip's ±8.64e15 boundary.
//
// Every date here is built through `Date.UTC` or a time value, never through
// the LOCAL field constructor: `new Date(2020, 0, 1)` names a different instant
// in every timezone, and a pinned expectation may not depend on where the test
// ran. The local form is exercised in date_setters.js, in a relation that holds
// in every zone.

// ---- the argument forms -----------------------------------------------------
console.log(new Date(0).toISOString());
console.log(new Date(1577836800000).toISOString());
console.log(new Date("2020-01-01T00:00:00Z").toISOString());
console.log(new Date(Date.UTC(2020, 0, 1)).toISOString());

// 21.4.2.1 step 3.a: a Date argument is copied through its [[DateValue]].
const source = new Date(Date.UTC(1999, 11, 31, 23, 59, 59, 999));
console.log(new Date(source).toISOString());
console.log(new Date(source).getTime() === source.getTime());

// ToPrimitive with no hint runs on any other single argument, so a wrapper and
// an object with a valueOf both work.
console.log(new Date({ valueOf() { return 86400000; } }).toISOString());

// ---- MakeFullYear (21.4.1.32) -----------------------------------------------
console.log(Date.UTC(0, 0, 1));
console.log(Date.UTC(99, 0, 1));
console.log(new Date(Date.UTC(70, 0, 1)).toISOString());
console.log(new Date(Date.UTC(100, 0, 1)).toISOString());
console.log(new Date(Date.UTC(-1, 0, 1)).toISOString());
// The mapping is applied to the TRUNCATED year, so 99.9 is 1999.
console.log(new Date(Date.UTC(99.9, 0, 1)).toISOString());

// ---- field overflow, both directions ----------------------------------------
console.log(new Date(Date.UTC(2020, 12, 32)).toISOString());
console.log(new Date(Date.UTC(2020, -1)).toISOString());
console.log(new Date(Date.UTC(2020, 0, 0)).toISOString());
console.log(new Date(Date.UTC(2020, -13, 1)).toISOString());
console.log(new Date(Date.UTC(2020, 0, 1, 25)).toISOString());
console.log(new Date(Date.UTC(2020, 0, 1, 0, 0, 0, -1)).toISOString());
console.log(new Date(Date.UTC(2020, 1, 29)).toISOString());
console.log(new Date(Date.UTC(2021, 1, 29)).toISOString());
console.log(new Date(Date.UTC(1900, 1, 29)).toISOString());

// ---- Date.UTC's own argument defaults (21.4.3.4) ----------------------------
console.log(Date.UTC(2020));
console.log(Number.isNaN(Date.UTC()));
console.log(Number.isNaN(Date.UTC(NaN, 0)));

// ---- TimeClip (21.4.1.31) ---------------------------------------------------
console.log(new Date(8640000000000000).getTime());
console.log(new Date(-8640000000000000).getTime());
console.log(Number.isNaN(new Date(8640000000000001).getTime()));
console.log(Number.isNaN(new Date(-8640000000000001).getTime()));
console.log(Number.isNaN(new Date(Infinity).getTime()));
console.log(new Date(1.9).getTime());
// -0 is folded into +0, so the time value never prints as "-0".
console.log(Object.is(new Date(-0.5).getTime(), 0));

// ---- the single-argument conversions ----------------------------------------
console.log(new Date(null).getTime());
console.log(new Date(true).getTime());
console.log(Number.isNaN(new Date(undefined).getTime()));
console.log(Number.isNaN(new Date("").getTime()));
console.log(Number.isNaN(new Date("not a date").getTime()));

// ---- `Date(...)` without `new` (21.4.2.2) -----------------------------------
// Every argument is ignored and the answer is a String naming the current time.
// The value cannot be pinned, but three things about it can: that it is a
// string, that no `new` happened, and that the arguments really were ignored —
// two calls whose years are two decades apart land milliseconds apart, which is
// only true if neither year reached the result.
//
// The empty-argument spellings of both forms are absent on purpose:
// oracle_test.cpp bans a clock read from a byte-pinned case, and it recognises
// a clock read by the empty argument list — in the source text, comments
// included, so they cannot be named here either. A call WITH arguments reads
// the same clock, so nothing here lets a clock value reach the output.
console.log(typeof Date(2020, 0, 1));
console.log(Date(2020, 0, 1) instanceof Date);
{
  const a = Date.parse(Date(2020, 0, 1));
  const b = Date.parse(Date(1999, 5, 5));
  console.log(Number.isFinite(a), Math.abs(a - b) < 60000);
}

// ---- what a Date IS ---------------------------------------------------------
const d = new Date(0);
console.log(typeof d);
console.log(d instanceof Date);
console.log(d.constructor === Date);
console.log(Object.prototype.toString.call(d));
console.log(Object.keys(d).length, Object.getOwnPropertyNames(d).length);
console.log(JSON.stringify({ ...d }));
// [[DateValue]] is an internal slot, so a Date carries no own property at all
// and an ordinary assignment still works beside it.
d.tag = "x";
console.log(Object.keys(d).join(","), d.getTime());
