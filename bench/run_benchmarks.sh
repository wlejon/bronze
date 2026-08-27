#!/usr/bin/env bash
# ==============================================================================
# Bronze Benchmark Suite Runner
#
# Deterministic benchmark runner across execution modes:
#   1. Compiled with Inference (default AOT with native layout proofs)
#   2. Compiled without Inference (--no-infer uniform dynamic convention)
#   3. Host Compiled vs Interpreted (for Bro WebGL/Scene render benchmarks)
#   4. Pinned Node.js Baseline Comparison (Node v24.2.0 out-of-band reference)
#
# HARD RULES (CLAUDE.md):
#   - Automated runner MUST NEVER invoke node.
#   - No sleep/poll loops.
#   - Median of >= 5 timed runs (or >= 3 under --quick), warmup run discarded.
#   - Produces machine-readable JSON lines and a clean human Markdown table.
# ==============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

RUNS=5
FILTER=""
PURE_ONLY=0
RENDER_ONLY=0
ALLOW_DEBUG=0
JSON_ONLY=0
INFER_ONLY=0
NOINFER_ONLY=0
DO_PROFILE=0
USE_CACHED=0
JSONL_OUT="$SCRIPT_DIR/results.jsonl"
BASELINES_FILE="$SCRIPT_DIR/node_baselines.json"

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Options:
  --quick            Fast iteration mode (3 runs, infer-only, runs in ~30s)
  --infer-only       Run only default inference mode (skip --no-infer builds/runs)
  --no-infer-only    Run only --no-infer mode
  --profile          Capture top ABI helper calls and IC misses per benchmark
  --cached           Reuse existing compiled binaries if newer than source
  --runs N           Number of timed runs per benchmark (default: 5, warmup discarded)
  --filter PATTERN   Run only benchmarks matching PATTERN (case-insensitive)
  --pure-only        Run only pure-compute benchmarks (no GL/DOM)
  --render-only      Run only render scene benchmarks (requires Bro host)
  --allow-debug      Allow running benchmarks with a Debug (non-Release) bronze binary
  --json             Print only machine-readable JSON lines to stdout
  --jsonl-out FILE   Output file for JSON lines (default: bench/results.jsonl)
  -h, --help         Show this help message

Environment Variables:
  BRONZE_CLI         Path to bronze CLI executable (auto-detected if unset)
  BRO_DIR            Path to Bro working tree / build (auto-detected if unset)
EOF
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --quick)
            RUNS=3
            INFER_ONLY=1
            PURE_ONLY=1
            shift
            ;;
        --infer-only)
            INFER_ONLY=1
            shift
            ;;
        --no-infer-only)
            NOINFER_ONLY=1
            shift
            ;;
        --profile)
            DO_PROFILE=1
            shift
            ;;
        --cached|--no-rebuild)
            USE_CACHED=1
            shift
            ;;
        --runs)
            RUNS="$2"
            shift 2
            ;;
        --filter)
            FILTER="$2"
            shift 2
            ;;
        --pure-only)
            PURE_ONLY=1
            shift
            ;;
        --render-only)
            RENDER_ONLY=1
            shift
            ;;
        --allow-debug)
            ALLOW_DEBUG=1
            shift
            ;;
        --json)
            JSON_ONLY=1
            shift
            ;;
        --jsonl-out)
            JSONL_OUT="$2"
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage
            ;;
    esac
done

to_win_path() {
    local p="$1"
    if [[ "$p" =~ ^/mnt/([a-zA-Z])/(.*) ]]; then
        echo "${BASH_REMATCH[1]}:/${BASH_REMATCH[2]}"
    elif [[ "$p" =~ ^/([a-zA-Z])/(.*) ]]; then
        echo "${BASH_REMATCH[1]}:/${BASH_REMATCH[2]}"
    else
        echo "$p"
    fi
}

# --- Locate Bronze CLI ---
find_bronze_cli() {
    if [[ -n "${BRONZE_CLI:-}" && -f "$BRONZE_CLI" ]]; then
        echo "$BRONZE_CLI"
        return 0
    fi
    for cand in \
        "$ROOT_DIR/build/dev/src/cli/bronze.exe" \
        "$ROOT_DIR/build/dev/src/cli/bronze" \
        "$ROOT_DIR/build/src/cli/bronze.exe" \
        "$ROOT_DIR/build/src/cli/bronze" \
        "$ROOT_DIR/build/Release/bronze.exe" \
        "$ROOT_DIR/build/Release/bronze"
    do
        if [[ -f "$cand" ]]; then
            echo "$cand"
            return 0
        fi
    done
    echo ""
}

BRONZE_BIN="$(find_bronze_cli)"
if [[ -z "$BRONZE_BIN" ]]; then
    echo "Error: bronze CLI not found. Build it first via ./dev.cmd cmake --build --preset dev" >&2
    exit 1
fi

# --- Detect Build Type of Bronze CLI ---
detect_build_type() {
    local bin="$1"
    local ver_out
    ver_out="$("$bin" version 2>/dev/null || echo "")"
    if [[ "$ver_out" =~ Release ]]; then
        echo "Release"
        return 0
    elif [[ "$ver_out" =~ Debug ]]; then
        echo "Debug"
        return 0
    fi
    if [[ "$bin" =~ [Rr]elease || "$bin" =~ /dev/ ]]; then
        echo "Release"
        return 0
    fi
    echo "Debug"
}

BUILD_TYPE="$(detect_build_type "$BRONZE_BIN")"

if [[ "$BUILD_TYPE" != "Release" && $ALLOW_DEBUG -eq 0 ]]; then
    echo "Error: $BRONZE_BIN is a $BUILD_TYPE binary. Benchmarks require a Release build for build-type truth." >&2
    echo "       Rebuild with: ./dev.cmd cmake --preset dev -DBRONZE_WITH_LLVM=ON && ./dev.cmd cmake --build --preset dev" >&2
    echo "       Or pass --allow-debug to benchmark this build anyway." >&2
    exit 1
fi

# --- Locate Bro Host Executables ---
find_bro_dir() {
    if [[ -n "${BRO_DIR:-}" && -d "$BRO_DIR" ]]; then
        echo "$BRO_DIR"
        return 0
    fi
    for cand in \
        "$ROOT_DIR/../bro" \
        "/mnt/d/projects/bro" \
        "/d/projects/bro" \
        "D:/projects/bro"
    do
        if [[ -d "$cand" ]]; then
            echo "$cand"
            return 0
        fi
    done
    echo ""
}

BRO_PATH="$(find_bro_dir)"
BRO_HOST_SCENEGRAPH=""
BRO_HOST_WILD=""
BRO_HOST_INSTANCED=""
BRO_HEADLESS=""

if [[ -n "$BRO_PATH" ]]; then
    for cand in "$BRO_PATH/build/Release/bro-bronze-host.exe" "$BRO_PATH/build/bro-bronze-host"; do
        [[ -f "$cand" ]] && BRO_HOST_SCENEGRAPH="$cand" && break
    done
    for cand in "$BRO_PATH/build/Release/bro-bronze-host-wild.exe" "$BRO_PATH/build/bro-bronze-host-wild"; do
        [[ -f "$cand" ]] && BRO_HOST_WILD="$cand" && break
    done
    for cand in "$BRO_PATH/build/Release/bro-bronze-host-instanced.exe" "$BRO_PATH/build/bro-bronze-host-instanced"; do
        [[ -f "$cand" ]] && BRO_HOST_INSTANCED="$cand" && break
    done
    for cand in "$BRO_PATH/build/Release/bro-headless.exe" "$BRO_PATH/build/bro-headless"; do
        [[ -f "$cand" ]] && BRO_HEADLESS="$cand" && break
    done
fi

# --- Python timing and statistics helper ---
# --- Timing helper ---
#
# WHAT IS MEASURED IS WHAT THE FIXTURE PRINTED. Every pure-compute fixture times
# its own compute region through bench/harness.js and writes
# `[bench] <region> ms=<f>` to stderr; this reads that line and never looks at a
# process wall clock. Process startup is not a fact about the compiler — an
# empty bronze program costs ~5.9 ms on this box and an empty node program ~31 —
# and including it made a fixture's number depend mostly on which engine ran it.
#
# MODE is `self` (the default, and the only mode any fixture in bench/ uses) or
# `wall`. `wall` measures the whole process and is for the Bro render scenes
# only: those are host applications from another tree, they cannot be taught to
# self-report from here, and their rows are labelled as including startup.
time_command_json() {
    local cmd_json="$1"
    local runs="$2"
    local mode="${3:-self}"
    python3 -c '
import sys, subprocess, time, statistics, json, re

cmd = json.loads(sys.argv[1])
runs = int(sys.argv[2])
mode = sys.argv[3]

LINE = re.compile(r"^\[bench\] (\S+) ms=([0-9.]+)", re.M)

def elapsed(p, wall_ms):
    """The number this run contributes, in ms."""
    if mode == "wall":
        return wall_ms
    found = LINE.findall(p.stderr or "")
    if not found:
        return None
    # A fixture with several timed regions (typed_array_loop, call_chain_kernel)
    # contributes their SUM: the report has one row per fixture, and the row is
    # the compute the fixture did. interleave.py is the tool that keeps regions
    # apart when the difference between them is the question.
    return sum(float(v) for _, v in found)

# Warmup run (discarded)
try:
    p_warm = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    if p_warm.returncode != 0:
        print(json.dumps({"error": f"Warmup exit code {p_warm.returncode}: {p_warm.stderr.strip()}"}))
        sys.exit(0)
except Exception as e:
    print(json.dumps({"error": f"Warmup failed: {str(e)}"}))
    sys.exit(0)

timings = []
last_out = p_warm.stdout or ""
last_err = p_warm.stderr or ""
for _ in range(runs):
    t0 = time.perf_counter()
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    t1 = time.perf_counter()
    if p.returncode != 0:
        print(json.dumps({"error": f"Run exit code {p.returncode}: {p.stderr.strip()}"}))
        sys.exit(0)
    ms = elapsed(p, (t1 - t0) * 1000.0)
    if ms is None:
        print(json.dumps({"error": "no [bench] line on stderr: the fixture does not "
                                   "time itself through bench/harness.js"}))
        sys.exit(0)
    timings.append(ms)
    last_out = p.stdout or ""
    last_err = p.stderr or ""

timings.sort()
med = statistics.median(timings)
mn = min(timings)
mx = max(timings)
mean_val = statistics.mean(timings)
stdev_val = statistics.stdev(timings) if len(timings) > 1 else 0.0

# The checksum comes off STDOUT. The `[bench]` lines are on stderr and are the
# one deliberately nondeterministic thing a fixture prints, so they must never
# be mistaken for the output a run is identified by.
combined_lines = [line.strip()
                  for line in last_out.splitlines() + last_err.splitlines()
                  if line.strip() and not line.strip().startswith("[bench]")]
checksum_line = ""
for line in reversed(combined_lines):
    if "[console]" in line:
        line = line[line.find("[console]") + len("[console]"):].strip()
    if "checksum=" in line or line.startswith("APP ") or line.startswith("DRV "):
        checksum_line = line
        break
if not checksum_line and combined_lines:
    checksum_line = combined_lines[-1]

print(json.dumps({
    "median_ms": round(med, 2),
    "min_ms": round(mn, 2),
    "max_ms": round(mx, 2),
    "mean_ms": round(mean_val, 2),
    "stdev_ms": round(stdev_val, 2),
    "measured": mode,
    "timings": [round(t, 2) for t in timings],
    "output": checksum_line
}))
' "$cmd_json" "$runs" "$mode"
}

# --- Profile run helper ---
profile_command_json() {
    local cmd_json="$1"
    python3 -c '
import sys, subprocess, json, os

cmd = json.loads(sys.argv[1])
env = os.environ.copy()
env["BRONZE_PROFILE"] = "1"
env["BRONZE_IC_LOG"] = "1"
env["BRONZE_GC_LOG"] = "1"
env["WSLENV"] = "BRONZE_PROFILE:BRONZE_IC_LOG:BRONZE_GC_LOG"

try:
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=120, env=env)
    output = (p.stdout or "") + "\n" + (p.stderr or "")
    
    # Parse total helpers and top 3 helpers
    top_helpers = []
    total_helpers = 0
    in_profile = False
    for line in output.splitlines():
        line_s = line.strip()
        if "Total Dynamic ABI Helper Invocations:" in line_s:
            parts = line_s.split(":")
            if len(parts) > 1:
                try:
                    total_helpers = int(parts[1].strip())
                except:
                    pass
        elif "=== Bronze Runtime Profile" in line_s:
            in_profile = True
        elif in_profile and line_s.startswith("---"):
            continue
        elif in_profile and ("=== Bronze GC Log" in line_s or line_s.startswith("===")):
            in_profile = False
        elif in_profile and line_s:
            parts = line_s.split()
            if len(parts) >= 2 and parts[0].startswith("bronze_"):
                hname = parts[0]
                try:
                    hcount = int(parts[1])
                    top_helpers.append({"name": hname, "count": hcount})
                except:
                    pass

    # Parse top IC miss reason
    top_miss = ""
    in_miss = False
    for line in output.splitlines():
        line_s = line.strip()
        if "--- bronze_prop_get Misses by Reason" in line_s or "--- bronze_prop_set Misses by Reason" in line_s:
            in_miss = True
        elif in_miss and line_s.startswith("---"):
            continue
        elif in_miss and line_s.startswith("==="):
            in_miss = False
        elif in_miss and line_s and not top_miss:
            parts = line_s.split()
            if len(parts) >= 2:
                top_miss = f"{parts[0]} ({parts[1]})"
                in_miss = False

    res = {
        "total_helpers": total_helpers,
        "top_helpers": top_helpers[:3],
        "top_miss": top_miss
    }
    print(json.dumps(res))
except Exception as e:
    print(json.dumps({"error": str(e)}))
' "$cmd_json"
}

# --- Benchmark Registry ---
PURE_BENCHMARKS=(
    "three_math.js:Three.js Vector3/Matrix4 math loop against vendored three.js"
    "object_graph.js:Object-graph traversal, search, mutation, and cloning"
    "typed_array_crunch.js:Float64Array/Float32Array N-body physics & FFT crunch"
    "mesh_churn_2k.js:2k animated meshes scene graph updates & geometry churn"
    "instanced_mesh_churn.js:Three.js InstancedMesh 5,000 instances churn and color updates"
    "fib.js:Recursive fib(30) call overhead on dynamic function"
    "numeric_loop.js:10M-iteration float loop (proven-f64 arithmetic)"
    "property_access.js:1M iterations shape lookup & own-property IC dispatch"
    "proto_dispatch.js:3M iterations depth-3 inherited property read"
    "proto_dispatch_churn.js:3M iterations depth-3 read with object creation churn"
    "typed_array_loop.js:TypedArray element access vs plain array view"
)

mkdir -p "$(dirname "$JSONL_OUT")"
rm -f "$JSONL_OUT"

if [[ $JSON_ONLY -eq 0 ]]; then
    echo "================================================================================"
    echo "                      Bronze Compiler Benchmark Suite                           "
    echo "================================================================================"
    echo "Bronze CLI : $BRONZE_BIN"
    echo "Build Type : $BUILD_TYPE"
    echo "Runs / case: $RUNS (+ 1 warmup discarded)"
    echo "Mode       : $([ $INFER_ONLY -eq 1 ] && echo 'Inference only' || ([ $NOINFER_ONLY -eq 1 ] && echo 'No-Infer only' || echo 'Both (Infer & No-Infer)'))"
    if [[ $DO_PROFILE -eq 1 ]]; then
        echo "Profiling  : Enabled (capturing ABI helpers & IC misses)"
    fi
    if [[ -n "$BRO_PATH" ]]; then
        echo "Bro Tree   : $BRO_PATH"
    fi
    echo ""
fi

# --- Execute Pure Compute Benchmarks ---
if [[ $RENDER_ONLY -eq 0 ]]; then
    for item in "${PURE_BENCHMARKS[@]}"; do
        bench_file="${item%%:*}"
        bench_desc="${item#*:}"

        if [[ -n "$FILTER" && ! "$bench_file" =~ $FILTER && ! "$bench_desc" =~ $FILTER ]]; then
            continue
        fi

        js_path="$SCRIPT_DIR/$bench_file"
        if [[ ! -f "$js_path" ]]; then
            echo "Warning: Benchmark file $js_path not found, skipping." >&2
            continue
        fi

        exe_infer="$SCRIPT_DIR/${bench_file%.js}_infer.exe"
        exe_noinfer="$SCRIPT_DIR/${bench_file%.js}_noinfer.exe"
        win_js="$(to_win_path "$js_path")"
        win_exe_infer="$(to_win_path "$exe_infer")"
        win_exe_noinfer="$(to_win_path "$exe_noinfer")"

        # 1. Compile with default inference
        if [[ $NOINFER_ONLY -eq 0 ]]; then
            if [[ $USE_CACHED -eq 0 || ! -f "$exe_infer" || "$js_path" -nt "$exe_infer" || "$BRONZE_BIN" -nt "$exe_infer" ]]; then
                "$BRONZE_BIN" build "$win_js" -o "$win_exe_infer" >/dev/null 2>&1
                BUILD_STATUS_INFER=$?
                if [[ $BUILD_STATUS_INFER -ne 0 || ! -f "$exe_infer" ]]; then
                    echo "Error: Failed to build $bench_file (infer mode)" >&2
                    continue
                fi
            fi
        fi

        # 2. Compile without inference (--no-infer)
        if [[ $INFER_ONLY -eq 0 ]]; then
            if [[ $USE_CACHED -eq 0 || ! -f "$exe_noinfer" || "$js_path" -nt "$exe_noinfer" || "$BRONZE_BIN" -nt "$exe_noinfer" ]]; then
                "$BRONZE_BIN" build "$win_js" -o "$win_exe_noinfer" --no-infer >/dev/null 2>&1
                BUILD_STATUS_NOINFER=$?
                if [[ $BUILD_STATUS_NOINFER -ne 0 || ! -f "$exe_noinfer" ]]; then
                    echo "Error: Failed to build $bench_file (--no-infer mode)" >&2
                    continue
                fi
            fi
        fi

        # Time infer executable
        stats_infer="{}"
        profile_infer="{}"
        if [[ $NOINFER_ONLY -eq 0 && -f "$exe_infer" ]]; then
            cmd_infer_json="$(python3 -c "import json, sys; print(json.dumps([sys.argv[1]]))" "$exe_infer")"
            stats_infer="$(time_command_json "$cmd_infer_json" "$RUNS")"
            if [[ $DO_PROFILE -eq 1 ]]; then
                profile_infer="$(profile_command_json "$cmd_infer_json")"
            fi
        fi

        # Time no-infer executable
        stats_noinfer="{}"
        if [[ $INFER_ONLY -eq 0 && -f "$exe_noinfer" ]]; then
            cmd_noinfer_json="$(python3 -c "import json, sys; print(json.dumps([sys.argv[1]]))" "$exe_noinfer")"
            stats_noinfer="$(time_command_json "$cmd_noinfer_json" "$RUNS")"
        fi

        # Clean up if caching not requested
        if [[ $USE_CACHED -eq 0 ]]; then
            rm -f "$exe_infer" "$exe_noinfer"
        fi

        # Parse stats and combine with Node baseline
        record_json="$(python3 -c '
import sys, json, os

name = sys.argv[1]
desc = sys.argv[2]
st_inf = json.loads(sys.argv[3]) if sys.argv[3] else {}
st_noinf = json.loads(sys.argv[4]) if sys.argv[4] else {}
btype = sys.argv[5]
baselines_file = sys.argv[6]
prof_inf = json.loads(sys.argv[7]) if sys.argv[7] else {}

node_med = None
node_out = ""
if os.path.exists(baselines_file):
    try:
        with open(baselines_file, "r") as f:
            bdata = json.load(f)
            benches = bdata.get("benchmarks", {})
            if name in benches:
                node_med = benches[name].get("median_ms")
                node_out = benches[name].get("checksum", "")
    except:
        pass

if "error" in st_inf or "error" in st_noinf:
    err = st_inf.get("error", "") + " " + st_noinf.get("error", "")
    print(json.dumps({"name": name, "category": "pure-compute", "build_type": btype, "error": err.strip()}))
    sys.exit(0)

inf_med = st_inf.get("median_ms")
noinf_med = st_noinf.get("median_ms")
speedup = round(noinf_med / inf_med, 2) if (inf_med and noinf_med and inf_med > 0) else 1.0

vs_node = round(node_med / inf_med, 2) if (node_med and inf_med and inf_med > 0) else None

status = "N/A"
if vs_node is not None:
    if vs_node >= 1.05:
        status = "WIN"
    elif vs_node >= 0.95:
        status = "PARITY"
    else:
        status = "BEHIND"

record = {
    "name": name,
    "description": desc,
    "category": "pure-compute",
    "build_type": btype,
    "infer": st_inf if st_inf else None,
    "noinfer": st_noinf if st_noinf else None,
    "infer_speedup": speedup,
    "node_median_ms": node_med,
    "vs_node": vs_node,
    "status": status,
    "profile": prof_inf if prof_inf else None,
    "output_match": (st_inf.get("output") == st_noinf.get("output")) if (st_inf and st_noinf) else True
}
print(json.dumps(record))
' "$bench_file" "$bench_desc" "$stats_infer" "$stats_noinfer" "$BUILD_TYPE" "$BASELINES_FILE" "$profile_infer")"

        echo "$record_json" >> "$JSONL_OUT"

        if [[ $JSON_ONLY -eq 1 ]]; then
            echo "$record_json"
        fi
    done
fi

# --- Execute Render Benchmarks (Bro Host) ---
if [[ $PURE_ONLY -eq 0 && -n "$BRO_PATH" ]]; then
    # 1. Bro Bronze Host SceneGraph (compiled)
    if [[ -f "$BRO_HOST_SCENEGRAPH" && ( -z "$FILTER" || "render_scenegraph_host" =~ $FILTER || "scenegraph" =~ $FILTER ) ]]; then
        appdir="$BRO_PATH/src/bronze_host/app/appdir"
        win_appdir="$(to_win_path "$appdir")"
        cmd_host_json="$(python3 -c "import json, sys; print(json.dumps([sys.argv[1], sys.argv[2], '--headless', '--frames', '30']))" "$BRO_HOST_SCENEGRAPH" "$win_appdir")"
        stats_render="$(time_command_json "$cmd_host_json" "$RUNS" wall)"

        record_json="$(python3 -c '
import sys, json
st = json.loads(sys.argv[1])
btype = sys.argv[2]
record = {
    "name": "render_scenegraph_host",
    "description": "bro-bronze-host Three.js scenegraph (30 frames)",
    "category": "render-compiled",
    "build_type": btype,
    "infer": st,
    "noinfer": None,
    "infer_speedup": 1.0,
    "node_median_ms": None,
    "vs_node": None,
    "status": "HOST",
    "output_match": True
}
print(json.dumps(record))
' "$stats_render" "$BUILD_TYPE")"
        echo "$record_json" >> "$JSONL_OUT"
        if [[ $JSON_ONLY -eq 1 ]]; then
            echo "$record_json"
        fi
    fi

    # 2. Bro Bronze Host Wild Orbit (compiled)
    if [[ -f "$BRO_HOST_WILD" && ( -z "$FILTER" || "render_wild_orbit_host" =~ $FILTER || "wild" =~ $FILTER || "orbit" =~ $FILTER ) ]]; then
        appdir="$BRO_PATH/tests/bronze_host/appdir_wild"
        win_appdir="$(to_win_path "$appdir")"
        cmd_wild_json="$(python3 -c "import json, sys; print(json.dumps([sys.argv[1], sys.argv[2], '--headless', '--frames', '30']))" "$BRO_HOST_WILD" "$win_appdir")"
        stats_wild="$(time_command_json "$cmd_wild_json" "$RUNS" wall)"

        record_json="$(python3 -c '
import sys, json
st = json.loads(sys.argv[1])
btype = sys.argv[2]
record = {
    "name": "render_wild_orbit_host",
    "description": "bro-bronze-host-wild full Three.js scene + textures + OrbitControls (30 frames)",
    "category": "render-compiled",
    "build_type": btype,
    "infer": st,
    "noinfer": None,
    "infer_speedup": 1.0,
    "node_median_ms": None,
    "vs_node": None,
    "status": "HOST",
    "output_match": True
}
print(json.dumps(record))
' "$stats_wild" "$BUILD_TYPE")"
        echo "$record_json" >> "$JSONL_OUT"
        if [[ $JSON_ONLY -eq 1 ]]; then
            echo "$record_json"
        fi
    fi

    # 3. Bro Bronze Host Instanced Mesh (compiled)
    if [[ -f "$BRO_HOST_INSTANCED" && ( -z "$FILTER" || "render_instanced_host" =~ $FILTER || "instanced" =~ $FILTER ) ]]; then
        appdir="$BRO_PATH/tests/bronze_host/appdir_instanced"
        win_appdir="$(to_win_path "$appdir")"
        cmd_inst_json="$(python3 -c "import json, sys; print(json.dumps([sys.argv[1], sys.argv[2], '--headless', '--frames', '30']))" "$BRO_HOST_INSTANCED" "$win_appdir")"
        stats_inst="$(time_command_json "$cmd_inst_json" "$RUNS" wall)"

        record_json="$(python3 -c '
import sys, json
st = json.loads(sys.argv[1])
btype = sys.argv[2]
record = {
    "name": "render_instanced_host",
    "description": "bro-bronze-host-instanced 2,500 instances under load (30 frames)",
    "category": "render-compiled",
    "build_type": btype,
    "infer": st,
    "noinfer": None,
    "infer_speedup": 1.0,
    "node_median_ms": None,
    "vs_node": None,
    "status": "HOST",
    "output_match": True
}
print(json.dumps(record))
' "$stats_inst" "$BUILD_TYPE")"
        echo "$record_json" >> "$JSONL_OUT"
        if [[ $JSON_ONLY -eq 1 ]]; then
            echo "$record_json"
        fi
    fi

    # 4. Bro Headless Interpreted (QuickJS)
    if [[ -f "$BRO_HEADLESS" && -f "$SCRIPT_DIR/render_bench_interpreted.js" && ( -z "$FILTER" || "render_interpreted_bro" =~ $FILTER || "interpreted" =~ $FILTER ) ]]; then
        smoke_dir="$BRO_PATH/tests/_smoke_app"
        script_file="$SCRIPT_DIR/render_bench_interpreted.js"
        win_smoke="$(to_win_path "$smoke_dir")"
        win_script="$(to_win_path "$script_file")"
        cmd_interp_json="$(python3 -c "import json, sys; print(json.dumps([sys.argv[1], sys.argv[2], sys.argv[3]]))" "$BRO_HEADLESS" "$win_smoke" "$win_script")"
        stats_interp="$(time_command_json "$cmd_interp_json" "$RUNS" wall)"

        record_json="$(python3 -c '
import sys, json
st = json.loads(sys.argv[1])
btype = sys.argv[2]
record = {
    "name": "render_interpreted_bro",
    "description": "bro-headless interpreted QuickJS 3D scene (30 frames)",
    "category": "render-interpreted",
    "build_type": btype,
    "infer": st,
    "noinfer": None,
    "infer_speedup": 1.0,
    "node_median_ms": None,
    "vs_node": None,
    "status": "INTERP",
    "output_match": True
}
print(json.dumps(record))
' "$stats_interp" "$BUILD_TYPE")"
        echo "$record_json" >> "$JSONL_OUT"
        if [[ $JSON_ONLY -eq 1 ]]; then
            echo "$record_json"
        fi
    fi
fi

# --- Print Markdown Results Table ---
if [[ $JSON_ONLY -eq 0 ]]; then
    python3 -c '
import sys, json

jsonl_file = sys.argv[1]
do_prof = int(sys.argv[2])
records = []
try:
    with open(jsonl_file, "r") as f:
        for line in f:
            line = line.strip()
            if line:
                records.append(json.loads(line))
except Exception as e:
    print(f"Failed to read results: {e}")
    sys.exit(0)

if not records:
    print("No benchmark results generated.")
    sys.exit(0)

print("\n### Pure-Compute Benchmarks (Compiled Native vs Node.js Baseline)")
print("| Benchmark | Inferred (ms) | Node.js (ms) | vs Node | Status | No-Infer (ms) | Speedup (Infer) | Checksum / Output |")
print("|---|---|---|---|---|---|---|---|")

for r in records:
    if r.get("category") != "pure-compute":
        continue
    name = r.get("name", "")
    if "error" in r:
        err = r.get("error", "")
        print(f"| `{name}` | ERROR | - | - | ERROR | - | - | {err} |")
        continue
    inf_st = r.get("infer") or {}
    noinf_st = r.get("noinfer") or {}
    inf_med = "{:.2f}".format(inf_st.get("median_ms", 0.0)) if inf_st else "-"
    noinf_med = "{:.2f}".format(noinf_st.get("median_ms", 0.0)) if noinf_st else "-"
    speedup = "{:.2f}x".format(r.get("infer_speedup", 1.0)) if (inf_st and noinf_st) else "-"
    
    node_med = "{:.2f}".format(r["node_median_ms"]) if r.get("node_median_ms") is not None else "-"
    vs_node = "{:.2f}x".format(r["vs_node"]) if r.get("vs_node") is not None else "-"
    status = r.get("status", "-")
    if status == "WIN":
        status_badge = "**WIN**"
    elif status == "PARITY":
        status_badge = "PARITY"
    elif status == "BEHIND":
        status_badge = "BEHIND"
    else:
        status_badge = "-"

    out = inf_st.get("output", "") if inf_st else noinf_st.get("output", "")
    print(f"| `{name}` | **{inf_med}** | {node_med} | **{vs_node}** | {status_badge} | {noinf_med} | {speedup} | `{out}` |")

if do_prof:
    print("\n#### Profile Miss Breakdown (--profile)")
    print("| Benchmark | Total Helpers | Top ABI Helpers | Top IC Miss Reason |")
    print("|---|---|---|---|")
    for r in records:
        if r.get("category") != "pure-compute":
            continue
        name = r.get("name", "")
        prof = r.get("profile") or {}
        if not prof:
            continue
        tot = prof.get("total_helpers", 0)
        helpers = ", ".join([f"{h['name']}: {h['count']}" for h in prof.get("top_helpers", [])]) or "None"
        top_miss = prof.get("top_miss") or "None"
        print(f"| `{name}` | {tot:,} | {helpers} | {top_miss} |")

render_records = [r for r in records if r.get("category") != "pure-compute"]
if render_records:
    print("\n### Render Scene Benchmarks (Bro Host Execution)")
    print("| Scene Benchmark | Mode / Runtime | Median (ms) | Min (ms) | Max (ms) | Checksum / Status |")
    print("|---|---|---|---|---|---|")
    for r in render_records:
        name = r.get("name", "")
        if "error" in r:
            cat = r.get("category", "")
            err = r.get("error", "")
            print(f"| `{name}` | {cat} | ERROR | - | - | {err} |")
            continue
        st = r.get("infer") or {}
        cat = "Compiled Host" if "compiled" in r.get("category", "") else "Interpreted QuickJS"
        med = "{:.2f}".format(st.get("median_ms", 0.0))
        mn = "{:.2f}".format(st.get("min_ms", 0.0))
        mx = "{:.2f}".format(st.get("max_ms", 0.0))
        out = st.get("output", "")
        print(f"| `{name}` | {cat} | **{med}** | {mn} | {mx} | `{out}` |")

print("\nMachine-readable results saved to `" + jsonl_file + "`.")
' "$JSONL_OUT" "$DO_PROFILE"
fi
