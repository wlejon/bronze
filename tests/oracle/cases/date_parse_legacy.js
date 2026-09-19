// `Date.parse` beyond the 21.4.1.15 grammar (ECMA-262 21.4.3.2).
//
// The clause pins one grammar and lets "implementation-specific heuristics"
// read everything else. bronze's heuristics are V8's, carried over step for
// step (src/runtime/date_text.cpp), because every program was written against
// node's answers — including the ones nobody would design: "Jan 1" is the year
// 2001, "0" is the year 2000, "Feb 31 2020" is March 2nd, a leading space makes
// an ISO date local. This case pins those answers.
//
// Every line is zone-independent: a string with a written zone pins its value,
// a rejected string pins NaN, and a local-time form is pinned only through a
// relation with another local-time form or with the field constructor.
const value = (s) => {
  const v = Date.parse(s);
  return Number.isNaN(v) ? "NaN" : v;
};
const show = (list) => {
  for (const s of list) console.log(JSON.stringify(s), value(s));
};
const local = (s, y, mo, d, h = 0, mi = 0, sec = 0, ms = 0) => {
  console.log(JSON.stringify(s), Date.parse(s) === new Date(y, mo, d, h, mi, sec, ms).getTime());
};

// ---- month names, weekday names, commas, and the year rules ------------------
show([
  "Jan 1 2020 GMT", "Jan 1, 2020 GMT", "January 1, 2020 GMT", "1 Jan 2020 GMT",
  "1 January 2020 GMT", "Jan 2020 1 GMT", "2020 Jan 1 GMT", "2020 1 Jan GMT",
  "Mon Jan 01 2020 GMT", "Mon, Jan 01 2020 GMT", "Monday, January 1, 2020 GMT",
  "Sunday December 17 1995 GMT", "mayonnaise 1 2020 GMT", "sept 1 2020 GMT",
  "sep. 1 2020 GMT", "Jan-1-2020 GMT", "1-Jan-2020 GMT",
  // The KJS-era year rules: 0-49 → 2000s, 50-99 → 1900s, three digits as
  // written; a missing component fills as 1, so "Jan 1" is year 2001 and a
  // bare "0" is the year 2000.
  "Jan 1 20 GMT", "Jan 1 49 GMT", "Jan 1 50 GMT", "Jan 1 99 GMT", "Jan 1 100 GMT",
  "Jan 1 0 GMT", "Jan 1 GMT", "Jan 31 GMT", "0 GMT", "1 2 GMT", "1 2 3 GMT",
  "Jan 1 275760 GMT", "Jan 1 275761 GMT", "Jan 1 12345 GMT", "Jan 1 1234567 GMT",
  // A day past 31 fails; a day the month does not have carries forward.
  "Feb 31 2020 GMT", "Feb 30, 2020 GMT", "31 Feb 2020 GMT", "Jan 32 2020 GMT",
  "Jan 0 2020 GMT", "32 Jan 2020 GMT",
]);

// ---- numeric dates: slashes, dashes, dots, spaces ---------------------------
// Three components read as month/day/year unless the first cannot be a day.
show([
  "1/2/2020 GMT", "01/02/2020 GMT", "1/2/20 GMT", "1/2/99 GMT", "1/2/100 GMT",
  "1/2 GMT", "1/2/3 GMT", "12/31/2020 GMT", "12/32/2020 GMT", "13/1/2020 GMT",
  "2020/01/01 GMT", "2020/1/1 GMT", "1-2-2020 GMT", "1.5.2020 GMT", "1 2 2020 GMT",
  "2020 1 2 GMT", "2020 12 31 GMT", "2020 13 1 GMT", "99-1-1 GMT", "49-1-1 GMT",
  "2020-1-1 GMT", "2020-01-1 GMT", "2020-001-01 GMT", "12345-01-01 GMT",
  "1 2 3 4 GMT", "0 0 0 GMT", "1,2,2020 GMT",
]);

// ---- times: colons, am/pm, fractions, and what may follow a time ------------
show([
  "Jan 1 2020 10:00 GMT", "Jan 1 2020 10: GMT", "Jan 1 2020 10:: GMT", "Jan 1 2020 10::5 GMT",
  "Jan 1 2020 10:00:00.5 GMT", "Jan 1 2020 10:00:00.05 GMT", "Jan 1 2020 10:00:00.123456 GMT",
  "Jan 1 2020 10:00:00.1234567890 GMT", "Jan 1 2020 10:00:00.0001 GMT", "Jan 1 2020 10.5 GMT",
  "Jan 1 2020 10:00.5 GMT", "Jan 1 2020 000010:00 GMT", "10:00 Jan 1 2020 GMT",
  "Jan 1 2020 5:30 PM GMT", "5:30 PM Jan 1 2020 GMT", "Jan 1 2020 12am GMT",
  "Jan 1 2020 12pm GMT", "Jan 1 2020 13pm GMT", "Jan 1 2020 0:00 pm GMT",
  "Jan 1 2020 12:30:15.5 pm GMT", "Jan 1 2020 5pm GMT", "Jan 1 2020 12 pm GMT",
  "Jan 1 2020 10:00 am pm GMT", "pm Jan 1 2020 10:00 GMT",
  "Jan 1 2020 25:00 GMT", "Jan 1 2020 24:00 GMT", "Jan 1 2020 10:60 GMT",
  "Jan 1 2020 10:00:60 GMT", "Jan 1 2020 10:00:00 5 GMT", "Jan 1 2020 10:00:00:00 GMT",
  "Jan 1 2020 10 GMT", "Jan 1 2020 10:00 5 GMT", "Jan 1 2020 10:00x GMT",
  "1/2/2020, 10:00:00 AM GMT", "1/2/2020 10:00:00 PM GMT", "1995-12-17 03:24:00 PM GMT",
  "3:24:00 PM December 17 1995 GMT",
]);

// ---- zones: names, Z, and every offset spelling -----------------------------
show([
  "Jan 1 2020 10:00 UTC", "Jan 1 2020 10:00 UT", "Jan 1 2020 10:00 Z", "Jan 1 2020 10:00 z",
  "Jan 1 2020 10:00 EST", "Jan 1 2020 10:00 EDT", "Jan 1 2020 10:00 CST", "Jan 1 2020 10:00 CDT",
  "Jan 1 2020 10:00 MST", "Jan 1 2020 10:00 MDT", "Jan 1 2020 10:00 PST", "Jan 1 2020 10:00 PDT",
  "Jan 1 2020 10:00 CEST", "Jan 1 2020 10:00 UTx", "Jan 1 2020 10:00 GMTx", "Jan 1 2020 10:00 zz",
  "Jan 1 2020 10:00 z z", "Jan 1 2020 10:00 GMT EST", "Jan 1 2020 10:00 EST GMT",
  "Jan 1 2020 GMT", "Jan 1 2020 UTC+1", "Jan 1 2020 Z", "Jan 1 2020 EST", "Jan 1 2020 GMT-5",
  "Jan 1 2020 Z-5", "Jan 1 2020 -5", "Jan 1 2020 + 2",
  "Jan 1 2020 10:00 GMT+0100", "Jan 1 2020 10:00 GMT+1", "Jan 1 2020 10:00 GMT+01:30",
  "Jan 1 2020 10:00 GMT+5:3", "Jan 1 2020 10:00 GMT+5:", "Jan 1 2020 10:00 GMT+5:60",
  "Jan 1 2020 10:00 GMT+5: 30", "Jan 1 2020 10:00 GMT+530", "Jan 1 2020 10:00 GMT+05300",
  "Jan 1 2020 10:00 GMT+2500", "Jan 1 2020 10:00 GMT+9999", "Jan 1 2020 10:00 GMT+99",
  "Jan 1 2020 10:00 GMT+", "Jan 1 2020 10:00 GMT+ 5", "Jan 1 2020 10:00 GMT-",
  "Jan 1 2020 10:00 GMT-0", "Jan 1 2020 10:00 GMT-00000", "Jan 1 2020 10:00 GMT+5x",
  "Jan 1 2020 10:00 GMT+5-", "Jan 1 2020 10:00 GMT+5)", "Jan 1 2020 10:00 GMT+5 5",
  "Jan 1 2020 10:00 GMT+5 pm", "Jan 1 2020 10:00 GMT+5 Feb", "Jan 1 2020 10:00 GMT+5 GMT",
  "Jan 1 2020 10:00 GMT+5 +5", "Jan 1 2020 10:00 +0130", "Jan 1 2020 10:00 -05",
  "Jan 1 2020 10:00+01", "Jan 1 2020 10:00-5", "Jan 1 2020 10:00 -500", "Jan 1 2020 10:00 -50",
  "Jan 1 2020 10:00 -05:00", "Jan 1 2020 10:00 +12345", "Jan 1 2020 10:00 +123",
  "Jan 1 2020 10:00 +", "Jan 1 2020 10:00 - 5", "Jan 1 2020 10:00 -x", "Jan 1 2020 10:00 -Z",
  "Jan 1 2020 10:00 +05:", "Jan 1 2020 10:00 +05:7", "Jan 1 2020 10:00 +05:70",
  "Jan 1 2020 10:00 +23:59:", "Jan 1 2020 10:00 +24:00", "Jan 1 2020 10:00 GMT+999999",
  "1/1/2020 12:00:00 AM UTC", "Dec 25, 1995 13:30:00 GMT+0430", "Thu, 01 Jan 1970 00:00:00 GMT-0500",
  "12/17/1995 03:24:00 AM +05:00",
]);

// ---- the strings other engines print ----------------------------------------
show([
  "Wed Jan 01 2020 00:00:00 GMT+0000 (Coordinated Universal Time)",
  "Wed Jan 01 2020 00:00:00 GMT-0600 (Central Standard Time)",
  "Wed, 01 Jan 2020 00:00:00 +0000", "Wed, 01 Jan 2020 00:00:00 -0130",
  "01 Jan 2020 00:00:00 GMT", "Wed, 09 Aug 1995 00:00:00 GMT",
  "1 Jan 2020 10:00:00 GMT+0000 (GMT)", "Jan 1 2020 (a (nested) paren) 10:00 GMT",
  "Jan 1 2020 10:00 GMT (extra)", "Jan 1 2020 10:00 GMT+5 (x) y",
  "Jan 1 2020 10:00 GMT+5 (x) 5", "Jan 1 2020 10:00 GMT+5 (x))", "Jan 1 2020 10:00 GMT+0100 extra",
  "(x) Jan 1 2020 GMT", "(x)Jan 1 2020 GMT", "x Jan 1 2020 GMT", "x1 Jan 2020 GMT",
  "1x Jan 2020 GMT", "Jan 1x 2020 GMT", "Jan x 1 2020 GMT", "Jan 1 2020 foo GMT",
  "Jan 1 2020 GMT foo", "foo Jan 1 2020 GMT", "Jan 1 2020 10:00 foo",
]);

// ---- the ISO grammar's edges, and the strings that fall out of it -----------
show([
  "2020-01-01 10:00:00Z", "2020-01-01 10:00:00 Z", "2020-01-01 10:00:00+01:00",
  "2020-01-01 10:00:00 GMT+0100", "2020-01-01T10:00:00 GMT+0100", "2020-01-01Z",
  "2020-01-01 Z", "2020-01-01 GMT", "2020-01-01 UTC+1", "2020-01-01 EST", "2020-01Z", "2020Z",
  "2020T10:00Z", "2020-01T10:00Z", "2020-01-01+01:00", "2020-01-01T10:00Zx", "2020-01-01T10:00Z ",
  "2020-01-01T10:00 Z", "2020-01-01t10:00z", "2020-01-01T1000Z", "2020-01-01T10:00:00.Z",
  "2020-01-01T24:00:00.0Z", "2020-01-01T24:00:00.1Z", "2020-01-01T24:00:00.000001Z",
  "2020-01-01T10:00:00.1234567890Z", "2020-01-01T00:00:00+0100", "2020-01-01T00:00:00+01",
  "+002020", "-002020", "+002020-01-01T00:00:00Z", "-000000", "-000000-01-01 GMT",
  "+000000-01-01T00:00:00Z", "0000", "00000 GMT", "0001-01-01T00:00:00Z", "999999-01-01",
  "2020-13", "2020-00", "2020--01", "2020-01-01-", "2020-01-01-05", "2020-01-01 -05",
  "2020-01-01 10", "2020-01-01 10: GMT", "2020-01-01 10:00 pm GMT", "2020-01-01 pm",
  "2020-01-01 T10:00", "2020-09-01T 10:00", "2020-09-01TT10:00", "\t2020-01-01T00:00:00Z",
  "20200101 GMT", "202001 GMT", "20 GMT", "1e3", "T10:00", "Jan 1 2020 T10:00 GMT",
  "sep 1 2020T10:00", "1234567890", "17.12.1995", "2020-02-31",
]);
// Unicode spaces: a no-break space is whitespace to the tokenizer, but a line
// separator is a word character (every code point from 'A' up that is not a
// space is), so it is garbage next to a number. Labelled rather than echoed so
// the pinned bytes stay ASCII.
console.log("nbsp after", value("2020-01-01 GMT"), "nbsp between", value("2020 Jan 1 GMT"));
console.log("LS before", value(" 2020-01-01"), "LS after", value("2020-01-01 "));

// ---- local-time forms, pinned by relation -----------------------------------
local("Jan 1 2020", 2020, 0, 1);
local(" Jan 1 2020 ", 2020, 0, 1);
local("1/2/2020", 2020, 0, 2);
local("1/2/2020 10:00:00 PM", 2020, 0, 2, 22);
local("December 17, 1995 03:24:00", 1995, 11, 17, 3, 24);
local("1995-12-17T03:24:00", 1995, 11, 17, 3, 24);
local("2020-01-01 10:00:00", 2020, 0, 1, 10);
local("2020-1-1", 2020, 0, 1);
local("Feb 31 2020", 2020, 1, 31);
local("Jan 1", 2001, 0, 1);
local("0", 2000, 0, 1);
local("1 2 49", 2049, 0, 2);
local("1 2 050", 1950, 0, 2);
local("Jan 1 2020 10:00 am", 2020, 0, 1, 10);
local("1.5.2020", 2020, 0, 5);
local("Jan 1 2020 (unclosed GMT", 2020, 0, 1);
// A leading space turns an ISO date-only string from UTC into local, because
// the legacy loop reads it: the two differ by exactly the zone offset.
{
  const iso = Date.parse("2020-06-15");
  const spaced = Date.parse(" 2020-06-15");
  console.log((spaced - iso) / 60000 === new Date(spaced).getTimezoneOffset());
  console.log(Date.parse("2020-06-15 ") === spaced, Date.parse("2020-06-15 ") === spaced);
}
// `new Date(string)` is the same parse.
console.log(new Date("Jan 1 2020 10:00 GMT").getTime(), Number.isNaN(new Date("Jan 1 2020 foo").getTime()));
console.log(new Date("1/2/2020").getTime() === Date.parse("1/2/2020"));
