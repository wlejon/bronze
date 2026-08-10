import os
import sys
import time
import subprocess
import statistics
from pathlib import Path

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

def time_command(cmd, iterations=10):
    timings = []
    output = None
    for _ in range(iterations):
        t0 = time.perf_counter()
        proc = subprocess.run(cmd, capture_output=True, text=True, check=True)
        t1 = time.perf_counter()
        timings.append((t1 - t0) * 1000.0)  # ms
        if output is None:
            output = proc.stdout
    avg_ms = statistics.mean(timings)
    return avg_ms, output

def main():
    bench_dir = Path(__file__).parent.resolve()
    bronze_cli = get_bronze_path()
    
    benchmarks = ["fib.js", "numeric_loop.js"]
    results = []

    print(f"=== Bronze Microbenchmark Suite ===")
    print(f"Bronze CLI: {bronze_cli}\n")

    for bench_file in benchmarks:
        js_path = bench_dir / bench_file
        if not js_path.exists():
            print(f"Error: Benchmark file {bench_file} not found.", file=sys.stderr)
            continue
        
        exe_path = bench_dir / (js_path.stem + ".exe")
        
        # Build bronze executable
        build_cmd = [str(bronze_cli), "build", str(js_path), "-o", str(exe_path)]
        try:
            subprocess.run(build_cmd, check=True, capture_output=True, text=True)
        except subprocess.CalledProcessError as e:
            print(f"Failed to build {bench_file}: {e.stderr}", file=sys.stderr)
            continue

        # Measure Node
        node_time, node_out = time_command(["node", str(js_path)], iterations=10)

        # Measure Bronze
        bronze_time, bronze_out = time_command([str(exe_path)], iterations=10)

        speedup = node_time / bronze_time if bronze_time > 0 else 0.0
        results.append({
            "name": bench_file,
            "node_ms": node_time,
            "bronze_ms": bronze_time,
            "speedup": speedup,
            "output_match": (node_out == bronze_out)
        })

        if exe_path.exists():
            exe_path.unlink()

    print(f"| Benchmark | Node (ms) | Bronze (ms) | Speedup (Node/Bronze) | Stdout Match |")
    print(f"|---|---|---|---|---|")
    for r in results:
        print(f"| {r['name']} | {r['node_ms']:.2f} ms | {r['bronze_ms']:.2f} ms | {r['speedup']:.2f}x | {'YES' if r['output_match'] else 'NO'} |")

if __name__ == "__main__":
    main()
