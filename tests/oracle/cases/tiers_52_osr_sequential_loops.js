// A function with several loops one after another, and loops nested in
// each other. An OSR entry is compiled for one loop header at a time per
// function; a second loop reached while the first's entry is still compiling
// asks for its own (the first's entry cannot take a frame already past it).
// The function is also called again and again, so its baseline compile may be
// declined while a whole-function optimized compile is pending, and the
// frames keep running in Tier 0 meanwhile. Each loop carries values of a
// different kind out to the next one. Every tier must print the same thing.
function phases(n, k) {
  let a = 0;
  for (let i = 0; i < n; i++) a = (a + i * k) | 0;
  let b = 0.25;
  for (let i = 0; i < n; i++) b += ((a + i) & 7) / (i + 1);
  let s = '';
  for (let i = 0; i < n; i++) if (i % (n >> 2 || 1) === 0) s += String.fromCharCode(97 + ((a + i) & 15));
  const arr = [];
  for (let i = 0; i < n; i++) arr.push(i ^ k);
  let c = 0;
  for (let i = 0; i < arr.length; i++) {
    for (let j = 0; j < 4; j++) c += (arr[i] >> j) & 1;
  }
  let d = 0;
  outer: for (let i = 0; i < n; i++) {
    for (let j = 0; j < n; j++) {
      if (j * j > i) continue outer;
      if (i > n - 3) break outer;
      d += j;
    }
  }
  return a + ' ' + b.toFixed(6) + ' ' + s + ' ' + c + ' ' + d;
}

// One long call: each loop runs long enough to request its own entry.
console.log(phases(60000, 3));
// Many short calls: the function turns hot by calls while its loops are hot.
let acc = 0;
for (let r = 0; r < 400; r++) {
  const out = phases(200 + (r % 7), r);
  acc = (acc + out.length * 31 + out.charCodeAt(out.length - 1)) | 0;
}
console.log(acc);
// And long again, after whatever compiled for the short ones.
console.log(phases(40000, 5));
console.log(phases(1, 1));
console.log(phases(0, 9));
