// BLOCKED: `unsupported: Proxy.revocable (a revoked proxy must refuse every
// operation, and bronze's proxies have no revoked state to check)`.
//
// 28.2.2.1 pairs a proxy with a revoker: calling the revoker sets the proxy's
// [[ProxyTarget]] and [[ProxyHandler]] to null, and every internal method of
// a Proxy begins by throwing a TypeError when the handler is null (10.5.1
// step 2 and its siblings) — so after revocation, EVERY operation on the
// proxy refuses, reads and `in` tests alike. Before revocation the proxy is
// an ordinary proxy, and an empty handler forwards to the target.
//
// The revoker returns undefined (28.2.2.1.1 step 4), and revoking twice is a
// no-op rather than an error (step 1 returns early when [[RevocableProxy]] is
// already null) — both pinned below, because a stand-in that throws on the
// second call would look right until a program used the documented idiom of
// revoking defensively.
//
// Unblocking this means giving bronze's proxies a revoked state every
// internal-method entry checks, which is the check 10.5.1 step 2 names.
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
