// Stage R3, observability class (e): closures created inside a region.
//
// Creating one does not read the slot — but a closure that can be CALLED before
// the region ends can, so the call is where the region has to end. Both shapes
// are here: `build` only stores its closures, and every one of them sees the
// value the record holds when it is finally called, which is the write-back at
// the loop's exit being right. `buildAndCall` calls each closure in the same
// iteration that made it, through a value nothing enumerates, so the region
// ends at the call and each line reads the record.
function make() {
  let n = 0;
  const fns = [];

  function build(count) {
    for (let i = 0; i < count; i++) {
      n = n + 1;
      fns.push(function () { return n; });
    }
    return n;
  }

  function callEach() {
    let s = "";
    for (let k = 0; k < fns.length; k++) {
      s = s + fns[k]() + " ";
    }
    return s;
  }

  function buildAndCall(count) {
    let s = "";
    for (let i = 0; i < count; i++) {
      n = n + 10;
      const f = function () { return n; };
      s = s + f() + " ";
    }
    return s;
  }

  return { build: build, callEach: callEach, buildAndCall: buildAndCall,
           get: function () { return n; } };
}

const m = make();
console.log(m.build(3));
console.log(m.callEach());
console.log(m.buildAndCall(2));
console.log(m.get());
