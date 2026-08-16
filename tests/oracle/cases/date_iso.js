// `toISOString`, `toJSON`, `toUTCString` and `Date.parse` (ECMA-262 21.4.4.36,
// 21.4.4.37, 21.4.4.43 and 21.4.3.2) — the byte formats and the round trip
// between them.
//
// 21.4.1.15's grammar is the one bronze parses. Every string with an explicit
// offset or a `Z` names an instant that is the same in every timezone, and only
// those are pinned; the parser's local-time branch (a date-time form with no
// offset) is exercised only through a relation.

// ---- toISOString and the expanded year (21.4.4.36) --------------------------
console.log(new Date(0).toISOString());
console.log(new Date(1).toISOString());
console.log(new Date(-1).toISOString());
console.log(new Date(Date.UTC(2020, 6, 4, 13, 5, 9, 87)).toISOString());
console.log(new Date(Date.UTC(9999, 11, 31, 23, 59, 59, 999)).toISOString());
console.log(new Date(8640000000000000).toISOString());
console.log(new Date(-8640000000000000).toISOString());
console.log(new Date(Date.UTC(-1, 0, 1)).toISOString());
console.log(new Date(Date.UTC(10000, 0, 1)).toISOString());

// An invalid date has no ISO spelling, so 21.4.4.36 step 3 raises a RangeError
// — thrown, and therefore catchable.
try {
  new Date(NaN).toISOString();
  console.log("no throw");
} catch (e) {
  console.log(e instanceof RangeError, e instanceof Error, e.message);
}

// ---- toJSON (21.4.4.37) -----------------------------------------------------
// Step 3 answers null for a non-finite time value, which is why
// JSON.stringify of an invalid Date is "null" and not a thrown RangeError.
console.log(new Date(0).toJSON());
console.log(new Date(NaN).toJSON());
console.log(JSON.stringify(new Date(Date.UTC(2020, 0, 1))));
console.log(JSON.stringify(new Date(NaN)));
console.log(JSON.stringify({ when: new Date(0), bad: new Date(NaN) }));
console.log(JSON.stringify([new Date(86400000)]));
// toJSON reads `toISOString` off the RECEIVER, so an override is honoured.
{
  const d = new Date(0);
  d.toISOString = function () { return "OVERRIDDEN"; };
  console.log(JSON.stringify(d));
}

// ---- toUTCString (21.4.4.43) ------------------------------------------------
console.log(new Date(0).toUTCString());
console.log(new Date(Date.UTC(2020, 1, 29, 13, 5, 9, 999)).toUTCString());
console.log(new Date(-1).toUTCString());
console.log(new Date(Date.UTC(1900, 0, 1)).toUTCString());
console.log(new Date(NaN).toUTCString());
console.log(new Date(NaN).toString());
console.log(new Date(NaN).toDateString(), new Date(NaN).toTimeString());

// ---- Date.parse of the 21.4.1.15 grammar ------------------------------------
console.log(Date.parse("1970-01-01T00:00:00.000Z"));
console.log(Date.parse("2020-01-01T00:00:00Z"));
console.log(Date.parse("2020-01-01T00:00Z"));
console.log(Date.parse("2020-01-01"));
console.log(Date.parse("2020-01"));
console.log(Date.parse("2020"));
console.log(Date.parse("+275760-09-13T00:00:00.000Z"));
console.log(Date.parse("-271821-04-20T00:00:00.000Z"));
console.log(new Date(Date.parse("-000001-01-01T00:00:00Z")).toISOString());

// Offsets, in both directions and both spellings.
console.log(Date.parse("2020-01-01T00:00:00+00:00"));
console.log(Date.parse("2020-01-01T05:30:00+05:30"));
console.log(Date.parse("2020-01-01T00:00:00-05:00"));
console.log(Date.parse("2020-01-01T05:30:00+0530"));

// A fraction longer than three digits truncates; 24:00 is the end of the day.
console.log(Date.parse("2020-01-01T00:00:00.9999Z"));
console.log(Date.parse("2020-01-01T00:00:00.1Z"));
console.log(new Date(Date.parse("2020-01-01T24:00:00Z")).toISOString());

// Out of bounds and malformed both answer NaN, which 21.4.1.15's note makes one
// rule: an illegal value is a syntax error.
const rejected = ["2020-13-01", "2020-00-01", "2020-02-30", "2019-02-29", "2020-01-32",
                  "2020-01-01T25:00:00Z", "2020-01-01T24:00:01Z", "2020-01-01T00:60:00Z",
                  "-000000-01-01", "20-01-01", "2020-1-01", "", "   ", "not a date"];
let allNaN = true;
for (const text of rejected) {
  if (!Number.isNaN(Date.parse(text))) allNaN = false;
  if (!Number.isNaN(new Date(text).getTime())) allNaN = false;
}
console.log(allNaN);

// Beyond the representable range TimeClip answers NaN rather than wrapping.
console.log(Number.isNaN(Date.parse("+275760-09-14T00:00:00.000Z")));

// ---- the round trips --------------------------------------------------------
const samples = [0, 1, -1, 1577836800000, -2208988800000, 1593867909087, 8640000000000000];
let isoRound = true;
let utcRound = true;
let stringRound = true;
for (const tv of samples) {
  const d = new Date(tv);
  if (Date.parse(d.toISOString()) !== tv) isoRound = false;
  // toUTCString and toString both drop the milliseconds, so their round trip is
  // exact to the second — which is the comparison, not a weaker one.
  const second = tv - ((tv % 1000) + 1000) % 1000;
  if (Date.parse(d.toUTCString()) !== second) utcRound = false;
  if (Date.parse(d.toString()) !== second) stringRound = false;
}
console.log(isoRound, utcRound, stringRound);

// ---- ToString on a Date is `toString`, not `toISOString` --------------------
// And `toString` carries the local offset, so only its SHAPE can be pinned.
{
  const text = new Date(0).toString();
  console.log(/^[A-Z][a-z]{2} [A-Z][a-z]{2} \d{2} \d{4} \d{2}:\d{2}:\d{2} GMT[+-]\d{4}$/.test(text));
  console.log(new Date(0).toDateString().length, new Date(0).toTimeString().length);
  console.log(text.startsWith(new Date(0).toDateString()));
  console.log(text.endsWith(new Date(0).toTimeString()));
}

// A date-time string with NO offset is LOCAL (21.4.1.15), and a date-only one
// is UTC. The difference between the two is exactly the zone offset.
{
  const local = Date.parse("2020-06-15T00:00:00");
  const utc = Date.parse("2020-06-15T00:00:00Z");
  // `local` is the wall time read as UTC minus the offset, and
  // getTimezoneOffset reports the offset with the opposite sign — so the gap
  // between the two parses IS the reported offset, negated. June is outside
  // every zone's DST transition window, so this holds without a seam to worry
  // about.
  console.log((utc - local) / 60000 === -new Date(local).getTimezoneOffset());
  console.log(Date.parse("2020-06-15") === utc);
}
