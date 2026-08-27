// Validation case for the BRONZE_SAMPLE sampling profiler: two functions
// with IDENTICAL bodies, called at a 9:1 ratio, so their self-time split is
// 90/10 by construction. The sampler's report must land within a few points
// of that split for its attribution to be trusted.
//
// Compile and run:
//   bronze build bench/sampler_validation.js -o sampler_validation.exe
//   BRONZE_SAMPLE=1 ./sampler_validation.exe
//
// The two bodies are big enough that LLVM does not inline them into main
// (attribution to `main` instead of hot/cold is exactly the failure this
// case exists to catch), and the checksum keeps the loops from folding.

function hotPath(x) {
  let s = 0;
  for (let i = 0; i < 4000; i++) {
    s += (x * i + 1.5) % 7.3;
    s -= (s > 1e9) ? 1e9 : 0;
    s += Math.sqrt(i + x % 13.7);
  }
  return s;
}

function coldPath(x) {
  let s = 0;
  for (let i = 0; i < 4000; i++) {
    s += (x * i + 1.5) % 7.3;
    s -= (s > 1e9) ? 1e9 : 0;
    s += Math.sqrt(i + x % 13.7);
  }
  return s;
}

let checksum = 0;
const rounds = 30000;
// Nine of ten rounds run the hot body, one the cold: 90/10 by construction.
// The calls are SPREAD calls: bronze lowers those through the uniform
// dynamic convention (bronze_dynamic_call_spread → the callee's code
// pointer), which LLVM cannot inline through — so each body stays a separate
// native function and keeps its own symbol. A plain call, a two-armed
// ternary, and even a const-array element were all devirtualized and inlined
// into main when this case was first written, which erases the very
// attribution the case validates. Verify with BRONZE_PROFILE=1: the run must
// show ~30000 bronze_dynamic_call_spread invocations.
for (let r = 0; r < rounds; r++) {
  const args = [r];
  const f = (r % 10 === 9) ? coldPath : hotPath;
  checksum += f(...args);
}
console.log("sampler_validation checksum=" + Math.floor(checksum % 1e9));
