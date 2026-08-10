"""Bronze microbenchmark runner.

Builds each bench/*.js with the bronze CLI and times the produced native
executable. node is deliberately not invoked (see CLAUDE.md hard rules);
comparisons against other runtimes are done out-of-band if ever needed.
"""

import sys
import time
import subprocess
import statistics
from pathlib import Path

BENCHMARKS = ["fib.js", "numeric_loop.js", "property_access.js"]
ITERATIONS = 10


def get_bronze_path():
    candidates = [
        Path("build/dev/src/cli/bronze.exe"),
        Path("build/dev/src/cli/bronze"),
        Path("build/src/cli/bronze.exe"),
        Path("build/src/cli/bronze"),
    ]
    for cand in candidates:
        if cand.exists():
            return cand.resolve()
    return Path("bronze")


def time_command(cmd, iterations=ITERATIONS, timeout_s=60):
    timings = []
    output = None
    for _ in range(iterations):
        t0 = time.perf_counter()
        proc = subprocess.run(cmd, capture_output=True, text=True, check=True,
                              timeout=timeout_s)
        t1 = time.perf_counter()
        timings.append((t1 - t0) * 1000.0)  # ms
        if output is None:
            output = proc.stdout
    return statistics.mean(timings), min(timings), output


def main():
    bench_dir = Path(__file__).parent.resolve()
    bronze_cli = get_bronze_path()

    print("=== Bronze Microbenchmark Suite ===")
    print(f"Bronze CLI: {bronze_cli}\n")

    results = []
    for bench_file in BENCHMARKS:
        js_path = bench_dir / bench_file
        if not js_path.exists():
            print(f"Error: Benchmark file {bench_file} not found.", file=sys.stderr)
            continue

        exe_path = bench_dir / (js_path.stem + ".exe")
        build_cmd = [str(bronze_cli), "build", str(js_path), "-o", str(exe_path)]
        try:
            subprocess.run(build_cmd, check=True, capture_output=True, text=True)
        except subprocess.CalledProcessError as e:
            print(f"Failed to build {bench_file}: {e.stderr}", file=sys.stderr)
            continue

        try:
            avg_ms, min_ms, out = time_command([str(exe_path)])
        except subprocess.TimeoutExpired:
            print(f"{bench_file}: TIMED OUT", file=sys.stderr)
            continue
        finally:
            if exe_path.exists():
                exe_path.unlink()

        results.append({
            "name": bench_file,
            "avg_ms": avg_ms,
            "min_ms": min_ms,
            "output": out.strip(),
        })

    print("| Benchmark | Avg (ms) | Min (ms) | Output |")
    print("|---|---|---|---|")
    for r in results:
        print(f"| {r['name']} | {r['avg_ms']:.2f} | {r['min_ms']:.2f} | {r['output']} |")


if __name__ == "__main__":
    main()
