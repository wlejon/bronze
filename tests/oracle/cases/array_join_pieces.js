// Array.prototype.join builds its result in the units the pieces carry
// (builtin_array_transform.cpp JoinBuffer): Latin-1 until a unit past 0xFF,
// UTF-16 after. Each kind of piece, each separator kind, and the switch between
// encodings at every position are exercised; lengths and code units are
// printed so an encoding slip shows even where the text would look the same.

function show(s) {
  const units = [];
  for (let i = 0; i < s.length; i++) units.push(s.charCodeAt(i).toString(16));
  console.log(JSON.stringify(s), s.length, units.join(" "));
}

show([1, -0, 0.1, 1e21, -1e-7, NaN, Infinity, 2 ** 31, -(2 ** 31), 7].join());
show([true, false, null, undefined, "x", , "y"].join("-"));
show(["café", "ÿ", "naïve"].join("|"));
show(["a", "中", "b"].join());
show(["中", 1, true].join(""));
show([1, 2, 3].join("—"));
show(["😀", "\ud800", "\udfff", 5].join(","));
show(["Ā".slice(0, 1), "z"].join());
show([[1, [2, 3]], [], [null], "end"].join(";"));
show([{ toString() { return "objé"; } }, Symbol.iterator.description].join("+"));
show(new Array(4).join("ab"));
show([12345678901234567890, 0.000001, 123e-20].join(" "));

// A separator object runs its toString once.
let sepCalls = 0;
show(["p", "q", "r"].join({ toString() { sepCalls++; return "·"; } }));
console.log(sepCalls);

// An element's toString mutating the array under the join.
const arr = [1, { toString() { arr.length = 2; arr[1] = 9; return "m"; } }, 3, 4];
show(arr.join());

// An array-like receiver.
show(Array.prototype.join.call({ length: 3, 0: "x", 2: 2.5 }, "/"));
show(Array.prototype.join.call("é中", "."));
