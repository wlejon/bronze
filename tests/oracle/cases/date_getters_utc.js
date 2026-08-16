// The Date field getters, ECMA-262 21.4.4.2 - 21.4.4.19.
//
// The UTC half is pinned absolutely; the LOCAL half is pinned only through
// relations that hold in every timezone, because the machine's zone is not
// something a committed expectation may depend on.

const d = new Date(Date.UTC(2020, 6, 4, 13, 5, 9, 87));
console.log(d.getTime(), d.valueOf(), d.getTime() === d.valueOf());
console.log(d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate(), d.getUTCDay());
console.log(d.getUTCHours(), d.getUTCMinutes(), d.getUTCSeconds(), d.getUTCMilliseconds());
console.log(d.toISOString());

// ---- the negative half of the number line -----------------------------------
//
// A pre-1970 time value is negative, and every field of it comes out of a
// FLOOR division (5.2.5 modulo takes the sign of the divisor). Truncation
// toward zero is the classic bug and gets all seven of these wrong.
const before = new Date(-1);
console.log(before.getUTCFullYear(), before.getUTCMonth(), before.getUTCDate());
console.log(before.getUTCHours(), before.getUTCMinutes(), before.getUTCSeconds(),
            before.getUTCMilliseconds());
console.log(before.getUTCDay(), before.toISOString());

const moon = new Date(Date.UTC(1969, 6, 20, 20, 17, 40));
console.log(moon.getTime(), moon.toISOString(), moon.getUTCDay());

const deep = new Date(Date.UTC(1900, 0, 1));
console.log(deep.getTime(), deep.getUTCFullYear(), deep.getUTCDay());

// ---- the weekday, walked across a boundary ----------------------------------
// 1970-01-01 was a Thursday, so the seven days from 1969-12-29 run Mon..Sun.
let days = "";
for (let i = -3; i <= 3; i++) {
  days += new Date(i * 86400000).getUTCDay();
}
console.log(days);

// ---- an invalid date answers NaN from every getter --------------------------
const bad = new Date(NaN);
const names = ["getTime", "getUTCFullYear", "getUTCMonth", "getUTCDate", "getUTCDay",
               "getUTCHours", "getUTCMinutes", "getUTCSeconds", "getUTCMilliseconds",
               "getFullYear", "getMonth", "getDate", "getDay", "getHours", "getMinutes",
               "getSeconds", "getMilliseconds", "getTimezoneOffset"];
let allNaN = true;
for (const name of names) {
  if (!Number.isNaN(bad[name]())) allNaN = false;
}
console.log(allNaN);

// ---- the receiver check (21.4.4.1 thisTimeValue) ----------------------------
// Every member brand-checks, and the failure is a catchable TypeError rather
// than a read of whatever happened to be at that offset.
function callOn(receiver, name) {
  try {
    Date.prototype[name].call(receiver);
    return "no throw";
  } catch (e) {
    return e instanceof TypeError ? "TypeError" : "wrong error";
  }
}
console.log(callOn({}, "getTime"), callOn(null, "getUTCFullYear"), callOn(42, "toISOString"));
console.log(callOn(Object.create(Date.prototype), "getTime"));
console.log(callOn(Date.prototype, "getTime"));
console.log(callOn(new Date(0), "getTime"));

// ---- the local half, only where the zone cannot show ------------------------
const offset = d.getTimezoneOffset();
console.log(Number.isInteger(offset), offset >= -1440 && offset <= 1440);
// 21.4.1.24: LocalTime(t) is t - offset*60000, so a local field must agree with
// the UTC field of the shifted instant. This is the whole local path, checked
// without naming a zone.
const shifted = new Date(d.getTime() - offset * 60000);
console.log(d.getFullYear() === shifted.getUTCFullYear(),
            d.getMonth() === shifted.getUTCMonth(),
            d.getDate() === shifted.getUTCDate(),
            d.getDay() === shifted.getUTCDay());
console.log(d.getHours() === shifted.getUTCHours(),
            d.getMinutes() === shifted.getUTCMinutes(),
            d.getSeconds() === shifted.getUTCSeconds(),
            d.getMilliseconds() === shifted.getUTCMilliseconds());
// Milliseconds are never shifted by a zone: no offset has sub-second precision.
console.log(d.getMilliseconds() === d.getUTCMilliseconds());
