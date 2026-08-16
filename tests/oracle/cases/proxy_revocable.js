// 28.2.2.1 Proxy.revocable pairs a proxy with a revoker: calling the revoker
// sets the proxy's [[ProxyTarget]] and [[ProxyHandler]] to null, and every
// internal method of a Proxy begins by throwing a TypeError when the handler is
// null (10.5.1 step 2 and its siblings) — so after revocation EVERY operation
// on the proxy refuses, reads and `in` tests alike. Before revocation it is an
// ordinary proxy, and an empty handler forwards to the target.
//
// The revoker returns undefined (28.2.2.1.1 step 4), and revoking twice is a
// no-op rather than an error (step 1 returns early when [[RevocableProxy]] is
// already null) — both pinned below, because a stand-in that threw on the
// second call would look right until a program used the documented idiom of
// revoking defensively.
//
// `proxy_traps.js` carries the rest: the traps this case does not use, and the
// one operation revocation does NOT break (`typeof`).

const pair = Proxy.revocable({ x: 1 }, {});
console.log(typeof pair.proxy, typeof pair.revoke);
console.log(pair.proxy.x);
console.log(pair.revoke());
try {
  pair.proxy.x;
} catch (e) {
  console.log(e instanceof TypeError);
}
try {
  console.log("x" in pair.proxy);
} catch (e) {
  console.log(e instanceof TypeError);
}
console.log(pair.revoke());
