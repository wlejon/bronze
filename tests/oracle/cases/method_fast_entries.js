// Method sites with a fast entry (selfhost_merge.cpp kFastEntries) split on
// the receiver's kind: each site below sees the kind its entry takes and the
// kinds it must hand to the ordinary call, with fewer arguments than the
// entry's arity and with more.
function split(s, sep, lim) { return s.split(sep, lim); }
function split1(s, sep) { return s.split(sep); }
function split0(s) { return s.split(); }
function trim(s) { return s.trim(); }
function hex(n, r) { return n.toString(r); }
function str(n) { return n.toString(); }
function pop(a) { return a.pop(); }
function join(a, sep) { return a.join(sep); }
function join0(a) { return a.join(); }
function idx(x, v, from) { return x.indexOf(v, from); }
function idx1(x, v) { return x.indexOf(v); }
function sub(s, a, b, c) { return s.substring(a, b, c); }

const show = (v) => JSON.stringify(v);
for (let round = 0; round < 3; round++) {
  console.log(show(split("a,b,,c", ",")), show(split("a,b,,c", ",", 2)), show(split("abc", "", 2)));
  console.log(show(split("", ",")), show(split("", "")), show(split("aĀbĀ", "Ā")));
  console.log(show(split("x--y--", "--", undefined)), show(split("abc", "b", 0)), show(split("abc", "b", -1)));
  console.log(show(split1("a b  c", " ")), show(split1("abc", /b/)), show(split1("a1b2c", 1)));
  console.log(show(split1(new String("p,q"), ",")), show(split0("whole")), show(split1({ split: (s) => "own " + s }, "3")));
  console.log(show(split("a.b.c", ".", { valueOf() { return 2; } })));
  console.log(show(trim("  pad  ")), show(trim("none")), show(trim(" ﻿x\n")), show(trim(new String(" o "))));
  console.log(hex(255, 16), hex(-255, 16), hex(-0, 2), hex(1.5, 2), hex(35, 36), hex(255, undefined), hex(255, 10));
  console.log(hex(2 ** 53 + 2, 16), hex(255, 16.5), hex(NaN, 16), hex(Infinity, 2), hex(new Number(10), 2));
  try { hex(1, 37); } catch (e) { console.log(e.constructor.name); }
  try { hex(1, 1); } catch (e) { console.log(e.constructor.name); }
  console.log(str(0.1), str(1e21), str(-7), str(true), str([1, 2]), str({ toString() { return "own"; } }));
  const a = [1, 2, 3];
  console.log(pop(a), pop(a), pop(a), pop(a), show(a), pop({ pop: () => "own pop" }));
  const h = [1, , 3];
  h.length = 2;
  console.log(pop(h), h.length);
  console.log(join([1, null, "x", undefined], "-"), join([1, 2], undefined), join0([1, [2, 3]]), join({ join: (s) => "own " + s }, "+"));
  console.log(idx("hello", "l"), idx("hello", "l", 3), idx("hello", "l", -9), idx([1, 2, 1], 1, 1), idx1([1, 2], 2));
  console.log(idx1("a undefined b", undefined), idx1([undefined], undefined), idx({ indexOf: (v) => "own " + v }, 4));
  console.log(sub("abcdef", 4, 1, "extra"), sub("abcdef", 2), sub("abcdef", -3, 2), sub(new String("xyz"), 1));
}
