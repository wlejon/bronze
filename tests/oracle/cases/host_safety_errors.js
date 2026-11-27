// Host safety: deep proxy chains and unsupported user-reachable runtime paths
// raise catchable JS exceptions (RangeError / TypeError) rather than aborting
// the host process.

{
  let p = {};
  for (let i = 0; i < 1100; i++) {
    p = new Proxy(p, {});
  }
  let caught = "";
  try {
    Array.isArray(p);
  } catch (e) {
    caught = e.name;
  }
  console.log("deep-proxy-isArray:", caught);
}

{
  let caught = "";
  try {
    const s = new String("abc");
    s.valueOf = () => "override";
    JSON.stringify({ s });
  } catch (e) {
    caught = e.name;
  }
  console.log("json-stringify-wrapper:", caught);
}

{
  let caught = "";
  try {
    const r = /a/;
    r.exec = () => null;
    "a".match(r);
  } catch (e) {
    caught = e.name;
  }
  console.log("regexp-overridden-exec:", caught);
}

{
  let o = {};
  for (let i = 0; i < 1050; i++) {
    o = Object.create(o);
  }
  let caught = "";
  try {
    "x" in o;
  } catch (e) {
    caught = e.name;
  }
  console.log("deep-proto-in:", caught);
}
