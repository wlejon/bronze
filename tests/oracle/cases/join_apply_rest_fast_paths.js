// The direct paths of Array.prototype.join, Function.prototype.apply and rest
// parameters (builtin_array_transform.cpp, builtin_function.cpp, rt_spread.cpp):
// an array's own elements read straight out of it while no prototype holds an
// index, numbers written as integers when they are integral, and the answers
// that must not change when script makes a prototype hold an index halfway.

// --- join: numbers ---------------------------------------------------------
console.log([0, -0, 1, -1, 7 | 0, -7 | 0, 2 ** 31 - 1, -(2 ** 31)].join());
console.log([2 ** 53 - 1, -(2 ** 53 - 1), 2 ** 53, 2 ** 53 + 2, -(2 ** 53)].join());
console.log([1e20, 1e21, -1e21, 123456789012345680000, 0.5, -0.5, 1.5e-7].join());
console.log([NaN, Infinity, -Infinity, -0.0, 0 * -1, 3.0, 1 / 3].join("|"));
console.log([4294967295, 4294967296, -4294967296, 1e15 + 0.5].join(" "));

// --- join: lengths past the inline buffer, and widening late --------------
const many = [];
for (let i = 0; i < 300; i++) many.push(i * 1001);
const big = many.join();
console.log(big.length, big.slice(0, 30), big.slice(-30));
const wide = many.slice(0, 120).concat(["中"], many.slice(0, 5)).join("-");
console.log(wide.length, wide.slice(-40), wide.charCodeAt(wide.indexOf("中")));
console.log(new Array(200).fill("abc").join("").length);

// --- join: holes and a prototype that starts holding an index --------------
// (Object.prototype: bronze refuses index writes to Array.prototype itself.)
console.log([1, , 3, undefined, null, 5].join());
{
  const a = [1, , 3, , 5];
  let ran = false;
  a[0] = {
    toString() {
      if (!ran) Object.prototype[3] = "P3";
      ran = true;
      return "T";
    },
  };
  console.log(a.join());
  delete Object.prototype[3];
  console.log(a.join());
}
{
  const a = ["x", , "z"];
  console.log(a.join("/"));
  delete Object.prototype[1];
  console.log(a.join("/"));
}
{
  // An element's toString that shortens the array: later reads are undefined.
  const a = [1, 2, 3, 4, 5];
  a[1] = { toString() { a.length = 2; return "cut"; } };
  console.log(a.join(), a.length);
}
{
  // A subclass prototype holding an index is seen through a hole.
  class Sub extends Array {}
  Sub.prototype[0] = "S0";
  const s = Sub.from(["a", "b"]);
  delete s[0];
  console.log(s.join(), [, "b"].join());
}
console.log([[1, 2], [3, [4, 5]], "s", true, false].join(";"));

// --- apply: arrays, holes, arguments objects, array-likes ------------------
function show() {
  const parts = [];
  for (let i = 0; i < arguments.length; i++) parts.push(String(arguments[i]));
  return arguments.length + ":" + parts.join(",");
}
console.log(show.apply(null, [1, 2, 3]));
console.log(show.apply(null, [1, , 3]));
console.log(show.apply(null, new Array(3)));
console.log(show.apply(null, { length: 2, 0: "a", 1: "b" }));
console.log(show.apply(null, []));
console.log(show.apply(null, undefined));
{
  Object.prototype[1] = "proto1";
  console.log(show.apply(null, [1, , 3]));
  delete Object.prototype[1];
  console.log(show.apply(null, [1, , 3]));
}
function forward() { return show.apply(this, arguments); }
console.log(forward(), forward(1), forward(1, 2, 3, 4, 5, 6, 7, 8, 9));
console.log(forward(undefined), forward(undefined, undefined));
function forwardMutated(a) {
  arguments[1] = "set";
  return show.apply(null, arguments);
}
console.log(forwardMutated(1, 2, 3));
const nativeMax = Math.max.apply(Math, [3, 9, 4, -1]);
console.log(nativeMax, Math.min.apply(null, [5, , 2]));
console.log(String.fromCharCode.apply(null, [104, 105]));

// --- rest parameters ------------------------------------------------------
function rest(first, ...more) { return [first, more.length, more.join("+"), Array.isArray(more)]; }
console.log(rest().join(" "), "/", rest(1).join(" "), "/", rest(1, 2).join(" "));
console.log(rest(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17).join(" "));
function restAll(...xs) {
  xs.push("pushed");
  xs[30] = "far";
  return xs.length + " " + xs.slice(0, 6).join(",") + " " + xs[30];
}
console.log(restAll(), "|", restAll("a", "b", "c", "d", "e"));
console.log([10, 20, 30].map(function (...a) { return a.length + ":" + a[0] + ":" + a[1]; }).join(" "));
const objs = [];
function keep(...xs) { objs.push(xs); }
for (let i = 0; i < 50; i++) keep({ i }, [i], "s" + i, i * 0.5);
console.log(objs.length, objs[49][0].i, objs[49][1][0], objs[49][2], objs[49][3], objs[0].length);
