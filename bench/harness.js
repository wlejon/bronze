// The clock every benchmark in this directory is measured on.
//
// A benchmark process spends time on two unrelated things: starting up, and
// computing. Only the second is a fact about the compiler. On this box an
// empty bronze program costs about 5.9 ms of process wall and an empty node
// program about 31 ms, so a whole-process stopwatch reads a 20 ms kernel as
// 26 ms under one engine and 51 ms under the other — and the gap between those
// two floors is five times the thing being compared. Every number this suite
// publishes is therefore taken INSIDE the process, around the compute region
// and nothing else.
//
// `performance.now()` is the clock because it is monotonic and carries a
// sub-millisecond part; `Date.now()` is integer milliseconds by specification
// (ECMA-262 21.4.3.1) and cannot resolve any fixture here. bronze implements it
// (`src/runtime/builtin_performance.cpp`) and node has had it as a global since
// v16, so a fixture times itself identically under both.
//
// The measurement goes to STDERR and the fixture keeps printing its checksum to
// STDOUT unchanged, which is what keeps the two separable: stdout stays
// byte-comparable across engines and inference modes (`cmp` on two captured
// stdouts is the miscompile check), while the timing — the one deliberately
// nondeterministic thing a fixture prints — never enters that comparison.

// Time one region and return whatever it returned, so a fixture wraps its
// compute in `measure(...)` and prints its own stdout afterwards, exactly as it
// did before. Formatting and I/O stay outside the clock.
//
// `iters` is optional and is the count the region covers. Passing it adds a
// per-iteration figure, which is the only honest unit for a kernel whose whole
// job is one operation repeated — a millisecond total says nothing without the
// count beside it.
export function measure(name, run, iters) {
  const t0 = performance.now();
  const result = run();
  const ms = performance.now() - t0;
  report(name, ms, iters);
  return result;
}

// The line every harness in bench/tools parses. Fixed shape, one per region:
//
//   [bench] three_math ms=14.0231
//   [bench] mat4_kernel ms=295.6180 ns_per_iter=16.423
//
// A fixture with more than one region — `typed_array_loop` runs the same
// arithmetic over a typed array and over a plain one, and the comparison
// between the two IS the benchmark — calls `measure` once per region under
// distinct names, and prints one line each.
//
// Milliseconds carry four decimals because the shortest region here is under a
// millisecond and a rounded zero is not a measurement.
function report(name, ms, iters) {
  const per = iters ? ' ns_per_iter=' + ((ms * 1e6) / iters).toFixed(3) : '';
  console.error('[bench] ' + name + ' ms=' + ms.toFixed(4) + per);
}
