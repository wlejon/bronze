// A throw from a JS callback that a built-in calls — sort comparators, map and
// the rest — unwinds through the built-in's native frames and reaches the
// caller's handler. The built-in stops at the throw (each algorithm is
// `? Call(...)`, which returns the abrupt completion at once).

function boom(tag) {
  throw new Error(tag);
}

// sort: the comparator throws on its third call. 23.1.3.30 SortIndexedProperties
// reads every element before sorting and writes back only when sorting is done,
// so an array whose sort threw keeps its original contents.
var arr = [5, 3, 9, 1, 7];
var calls = 0;
try {
  arr.sort(function (a, b) {
    if (++calls === 3) boom("sort");
    return a - b;
  });
} catch (e) {
  console.log("sort threw " + e.message + " after " + calls + " calls");
}
console.log(arr.join(","));

var seen = [];
try {
  [1, 2, 3, 4].map(function (v) {
    seen.push(v);
    if (v === 3) boom("map");
    return v * 2;
  });
} catch (e) {
  console.log(e.message + " saw " + seen.join(","));
}

try {
  [1, 2, 3].forEach(function (v) {
    if (v === 2) throw v;
  });
} catch (v) {
  console.log("forEach threw " + v);
}

try {
  [1, 2, 3].reduce(function (a, v) {
    if (v === 3) boom("reduce at " + a);
    return a + v;
  });
} catch (e) {
  console.log(e.message);
}

try {
  [1, 2, 3].filter(function () { boom("filter"); });
} catch (e) {
  console.log(e.message);
}

try {
  [3, 4].find(function (v) { if (v === 4) boom("find"); return false; });
} catch (e) {
  console.log(e.message);
}

try {
  JSON.parse('{"a":1,"b":2}', function (k, v) {
    if (k === "b") boom("reviver");
    return v;
  });
} catch (e) {
  console.log(e.message);
}

try {
  "a-b-c".replace(/-/g, function () { boom("replace"); });
} catch (e) {
  console.log(e.message);
}

try {
  Array.from([1, 2], function (v) { if (v === 2) boom("from"); return v; });
} catch (e) {
  console.log(e.message);
}

try {
  new Map([[1, 1]]).forEach(function () { boom("map-forEach"); });
} catch (e) {
  console.log(e.message);
}

// A getter reached from a native operation.
var tricky = {};
Object.defineProperty(tricky, "x", { get: function () { boom("getter"); }, enumerable: true });
try {
  JSON.stringify(tricky);
} catch (e) {
  console.log(e.message);
}

// toString reached from a native coercion.
try {
  [{ toString: function () { boom("toString"); } }].join(",");
} catch (e) {
  console.log(e.message);
}

// Nested: a comparator that calls map whose callback throws; the comparator
// catches it itself, so the sort completes normally.
var nested = [3, 1, 2];
nested.sort(function (a, b) {
  try {
    [a].map(function () { boom("inner"); });
  } catch (e) {
    // handled here
  }
  return a - b;
});
console.log(nested.join(","));

// A throw from a callback two native layers down: sort inside a map callback.
try {
  [[2, 1], [4, 3]].map(function (pair) {
    return pair.sort(function (a, b) {
      if (a === 3 || b === 3) boom("deep");
      return a - b;
    });
  });
} catch (e) {
  console.log(e.message);
}

// Many throws through a comparator stay balanced.
var hits = 0;
for (var i = 0; i < 500; i++) {
  try {
    [2, 1].sort(function () { throw i; });
  } catch (v) {
    hits += v === i ? 1 : 0;
  }
}
console.log("comparator throws caught " + hits);
