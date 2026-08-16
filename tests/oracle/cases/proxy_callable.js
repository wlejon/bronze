// A Proxy over a CALLABLE target: 10.5.14 gives the proxy [[Call]] and
// [[Construct]] exactly when the target has them, and `typeof` reports that —
// "function" over a function target, "object" over anything else. The last two
// lines pin both halves, because a proxy that reports "function" and then
// refuses to be called would be worse than one that refuses outright.
//
// 10.5.12 [[Call]]: with no `apply` trap the call FORWARDS — same `this`, same
// arguments, and `Function.prototype.call` over the proxy reaches the target's
// body. With one, the trap is called as `trap(target, thisArgument,
// argumentsList)`, and `argumentsList` is a real ARRAY (step 6 is
// CreateArrayFromList), which the `Array.isArray` line pins: a trap that
// received an `arguments` object would look right until it called `.map`.
//
// 10.5.13 [[Construct]]: with no `construct` trap, `new` over the proxy
// constructs the TARGET, so the instance's prototype is the target's and
// `p instanceof Point` holds. With a trap, the trap's return value IS the
// instance, and step 9 makes a non-object return a TypeError rather than a
// silently discarded value.

function add(a, b) {
  return a + b;
}

const plain = new Proxy(add, {});
console.log(typeof plain, plain(1, 2), plain.call(null, 3, 4));

const logged = new Proxy(add, {
  apply: function (target, thisArg, args) {
    return "apply:" + args.length + ":" + target(args[0], args[1]);
  },
});
console.log(logged(2, 3));

let sawArray = false;
const probe = new Proxy(add, {
  apply: function (target, thisArg, args) {
    sawArray = Array.isArray(args);
    return thisArg === null ? "null-this" : thisArg.tag;
  },
});
console.log(probe.call(null, 1), sawArray);
console.log(probe.call({ tag: "receiver" }, 1));

function Point(x) {
  this.x = x;
}
const P = new Proxy(Point, {});
const p = new P(5);
console.log(p.x, p instanceof Point);

const Q = new Proxy(Point, {
  construct: function (target, args) {
    return { x: args[0] * 2, made: "trap" };
  },
});
const q = new Q(5);
console.log(q.x, q.made);

const R = new Proxy(Point, {
  construct: function () {
    return 5;
  },
});
try {
  new R(1);
} catch (e) {
  console.log(e instanceof TypeError);
}

const notCallable = new Proxy({}, {});
console.log(typeof notCallable);
try {
  notCallable();
} catch (e) {
  console.log(e instanceof TypeError);
}
