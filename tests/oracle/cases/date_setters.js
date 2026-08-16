// The Date setters, ECMA-262 21.4.4.20 - 21.4.4.31.
//
// Four things each of them decides and this case pins: what an OMITTED trailing
// argument means (the field is kept, not zeroed), what an EXPLICIT `undefined`
// means (ToNumber of it is NaN, and the date becomes invalid), what the return
// value is (the new time value, not the receiver), and what happens on an
// INVALID date — where `setFullYear` recovers from the epoch and every other
// setter stays NaN (21.4.4.21 step 3 against 21.4.4.22 step 4).

function at(iso) {
  return new Date(Date.parse(iso));
}

// ---- setTime (21.4.4.27) ----------------------------------------------------
{
  const d = new Date(0);
  const returned = d.setTime(1577836800000);
  console.log(returned, d.getTime(), returned === d.getTime());
  console.log(Number.isNaN(d.setTime(NaN)), Number.isNaN(d.getTime()));
  console.log(d.setTime(-0.5), Object.is(d.getTime(), 0));
  console.log(Number.isNaN(d.setTime(8640000000000001)));
}

// ---- the UTC field setters --------------------------------------------------
{
  const d = at("2020-01-01T00:00:00.000Z");
  console.log(d.setUTCFullYear(1999), d.toISOString());
  console.log(new Date(d.setUTCMonth(11)).toISOString());
  console.log(new Date(d.setUTCDate(31)).toISOString());
  console.log(new Date(d.setUTCHours(23)).toISOString());
  console.log(new Date(d.setUTCMinutes(59)).toISOString());
  console.log(new Date(d.setUTCSeconds(59)).toISOString());
  console.log(new Date(d.setUTCMilliseconds(999)).toISOString());
}

// ---- the trailing arguments -------------------------------------------------
{
  // Omitted: the field keeps the value it had.
  const kept = at("2020-06-15T10:20:30.400Z");
  kept.setUTCHours(1);
  console.log(kept.toISOString());

  // Supplied: every one of them is taken.
  const all = at("2020-06-15T10:20:30.400Z");
  all.setUTCHours(1, 2, 3, 4);
  console.log(all.toISOString());

  // Supplied as EXPLICIT undefined: ToNumber(undefined) is NaN, so the whole
  // date is invalid. "Present" is about the call, not about the value.
  const explicit = at("2020-06-15T10:20:30.400Z");
  explicit.setUTCHours(1, undefined);
  console.log(Number.isNaN(explicit.getTime()));

  // The same rule at the front: `setUTCHours()` with no arguments at all is
  // still ToNumber(undefined).
  const none = at("2020-06-15T10:20:30.400Z");
  none.setUTCHours();
  console.log(Number.isNaN(none.getTime()));

  const partial = at("2020-06-15T10:20:30.400Z");
  partial.setUTCMinutes(5, 6);
  console.log(partial.toISOString());

  const dateOnly = at("2020-06-15T10:20:30.400Z");
  dateOnly.setUTCFullYear(2021, 0);
  console.log(dateOnly.toISOString());
}

// ---- overflow normalises exactly as the constructor's fields do -------------
{
  const d = at("2020-01-31T00:00:00.000Z");
  console.log(new Date(d.setUTCMonth(1)).toISOString());
  const e = at("2020-01-01T00:00:00.000Z");
  console.log(new Date(e.setUTCDate(0)).toISOString());
  const f = at("2020-01-01T00:00:00.000Z");
  console.log(new Date(f.setUTCDate(366)).toISOString());
  const g = at("2020-01-01T12:00:00.000Z");
  console.log(new Date(g.setUTCHours(-1)).toISOString());
  const h = at("2020-01-01T00:00:00.000Z");
  console.log(new Date(h.setUTCMilliseconds(-1)).toISOString());
}

// ---- an invalid date --------------------------------------------------------
{
  // 21.4.4.21 step 3: setFullYear treats a NaN receiver as the epoch, so it is
  // the one setter that can revive an invalid date.
  const revived = new Date(NaN);
  revived.setUTCFullYear(2020, 0, 1);
  console.log(revived.toISOString());

  const revivedYearOnly = new Date(NaN);
  revivedYearOnly.setUTCFullYear(2020);
  console.log(revivedYearOnly.toISOString());

  // Every other setter answers NaN and leaves the date invalid.
  const stillBad = ["setUTCMonth", "setUTCDate", "setUTCHours", "setUTCMinutes",
                    "setUTCSeconds", "setUTCMilliseconds", "setMonth", "setDate",
                    "setHours", "setMinutes", "setSeconds", "setMilliseconds"];
  let allNaN = true;
  for (const name of stillBad) {
    const bad = new Date(NaN);
    if (!Number.isNaN(bad[name](1))) allNaN = false;
    if (!Number.isNaN(bad.getTime())) allNaN = false;
  }
  console.log(allNaN);
}

// ---- the receiver check -----------------------------------------------------
{
  let threw = "no throw";
  try {
    Date.prototype.setUTCFullYear.call({}, 2020);
  } catch (e) {
    threw = e instanceof TypeError ? "TypeError" : "wrong error";
  }
  console.log(threw);
}

// ---- the LOCAL setters, checked without naming a zone -----------------------
{
  // Seconds and milliseconds survive every zone and every DST rule: an offset
  // is a whole number of minutes, and even the half-hour shifts (Lord Howe
  // Island) leave these two alone. So they can be pinned absolutely.
  const d = new Date(Date.UTC(2020, 5, 15, 12, 0, 0, 0));
  console.log(d.setMilliseconds(123) === d.getTime(), d.getMilliseconds());
  d.setSeconds(45, 678);
  console.log(d.getSeconds(), d.getMilliseconds());
  console.log(d.getMilliseconds() === d.getUTCMilliseconds(),
              d.getSeconds() === d.getUTCSeconds());

  // The rest cannot be: a local wall time inside a DST gap does not exist, and
  // 21.4.1.25 leaves what it maps to implementation-defined. What DOES hold
  // everywhere is 21.4.1.24 itself — a local field is the UTC field of the
  // instant shifted by the offset — so that identity is what is checked.
  d.setFullYear(1985, 3, 12);
  d.setHours(6, 7, 8, 9);
  console.log(d.getSeconds(), d.getMilliseconds());
  const shifted = new Date(d.getTime() - d.getTimezoneOffset() * 60000);
  console.log(d.getFullYear() === shifted.getUTCFullYear(),
              d.getMonth() === shifted.getUTCMonth(),
              d.getDate() === shifted.getUTCDate(),
              d.getHours() === shifted.getUTCHours(),
              d.getMinutes() === shifted.getUTCMinutes());
}
