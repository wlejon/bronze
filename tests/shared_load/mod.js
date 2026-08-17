// The module of the shared-load test: compiled with `--emit-shared` into a
// DLL/.so/.dylib, opened at run time by tests/shared_load/harness.cpp, which
// says what each piece is proving.
//
// Everything here is written to be observable from OUTSIDE — every fact the
// module establishes, the host reads back through the embed API after at least
// one collection. A module and a host that did not share one heap would agree
// on every line the module prints itself and disagree on every line the host
// prints about it, which is the failure this file exists to make visible.

// Two host globals, from the manifest this module was compiled against
// (host_globals.txt). The harness registers them BEFORE entering the module,
// and the same two names are what it reads back out of the module's exported
// manifest symbol.
console.log("module: label=" + hostLabel + " tick=" + hostTick);

const store = { items: [], sum: 0 };

globalThis.record = function (name) {
  store.items.push(name);
  return store.items.length;
};

globalThis.readItems = function () {
  return store.items.join(",");
};

// Takes a typed array the HOST allocated and reads it as an ordinary JS view.
// If the host's heap and the module's heap were two heaps this would be
// reading whatever the module's collector had put at that address.
globalThis.sumBytes = function (view) {
  let total = 0;
  for (let i = 0; i < view.length; i++) total += view[i];
  store.sum = total;
  return total;
};

globalThis.lastSum = function () {
  return store.sum;
};

// The other direction: a view the MODULE allocated, handed out for the host to
// read through embed's typedArrayInfo. Int32 so every element prints as an
// exact integer and the pinned bytes carry no float formatting.
globalThis.makeSteps = function (n) {
  const v = new Int32Array(n);
  for (let i = 0; i < n; i++) v[i] = (i + 1) * 3;
  return v;
};

console.log("module: ready");
