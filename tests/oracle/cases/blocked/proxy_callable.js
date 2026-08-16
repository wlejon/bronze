// BLOCKED: `unsupported: Proxy over a callable target (calling through a Proxy
// is not built)`. It fires the moment such a proxy is CREATED, so even
// `typeof new Proxy(function () {}, {})` dies today.
//
// A Proxy is callable exactly when its target is (10.5.14 makes [[Call]] and
// [[Construct]] present only then), and `typeof` reports that: "function" over
// a function target, "object" over anything else. The last two lines pin both
// halves, because a proxy that reports "function" and then refuses to be called
// would be worse than one that refuses outright.
//
// 10.5.12 [[Call]]: with no `apply` trap, the call FORWARDS — same `this`, same
// arguments, and `Function.prototype.call` over the proxy reaches the target's
// body. With one, the trap is called as `trap(target, thisArgument,
// argumentsList)`, and `argumentsList` is a real ARRAY (step 6 is
// CreateArrayFromList), which the `Array.isArray` line pins: a trap that
// received an `arguments` object would look right until it called `.map`.
//
// 10.5.13 [[Construct]]: with no `construct` trap, `new` over the proxy
// constructs the TARGET with the proxy as newTarget — so the instance's
// prototype comes from `Get(proxy, "prototype")`, which forwards, and
// `p instanceof Point` holds. With a trap, the trap's return value is the
// instance, and step 9 makes a non-object return a TypeError rather than a
// silently discarded value.
//
// Unblocking this means giving bronze's proxies [[Call]] and [[Construct]],
// which is a change to the CALL path and not only to the property path: the
// call sites and `bronze_construct` both have to recognise a proxy and route
// through the trap or to the target, and `typeof` has to ask the target.
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
