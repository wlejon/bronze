#!/usr/bin/env bash
# ==============================================================================
# Bronze Benchmark Suite Runner
#
# Deterministic benchmark runner across execution modes:
#   1. Compiled AOT (harvest-guided build: --harvest-out -> run -> --feedback-in)
#   2. Host Compiled vs bro-headless (script compiled in process by bronze)
#      for Bro WebGL/Scene render benchmarks
#   3. Pinned Node.js Baseline Comparison (Node v24.2.0 out-of-band reference)
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
DO_PROFILE=0
USE_CACHED=0
JSONL_OUT="$SCRIPT_DIR/results.jsonl"
BASELINES_FILE="$SCRIPT_DIR/node_baselines.json"

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Options:
  --quick            Fast iteration mode (3 runs, pure-only, runs in ~30s)
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
            PURE_ONLY=1
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
            RENDER_ONLY=0
            shift
            ;;
        --render-only)
            RENDER_ONLY=1
            PURE_ONLY=0
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
    echo "       Rebuild with: ./dev.cmd cmake --preset dev && ./dev.cmd cmake --build --preset dev" >&2
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
    for cand in "$BRO_PATH/build/Release/bro-bronze-host.exe" "$BRO_PATH/build-release/bro-bronze-host" "$BRO_PATH/build/bro-bronze-host"; do
        [[ -f "$cand" ]] && BRO_HOST_SCENEGRAPH="$cand" && break
    done
    for cand in "$BRO_PATH/build/Release/bro-bronze-host-wild.exe" "$BRO_PATH/build-release/bro-bronze-host-wild" "$BRO_PATH/build/bro-bronze-host-wild"; do
        [[ -f "$cand" ]] && BRO_HOST_WILD="$cand" && break
    done
    for cand in "$BRO_PATH/build/Release/bro-bronze-host-instanced.exe" "$BRO_PATH/build-release/bro-bronze-host-instanced" "$BRO_PATH/build/bro-bronze-host-instanced"; do
        [[ -f "$cand" ]] && BRO_HOST_INSTANCED="$cand" && break
    done
    for cand in "$BRO_PATH/build/Release/bro-headless.exe" "$BRO_PATH/build-release/bro-headless" "$BRO_PATH/build/bro-headless"; do
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
BENCH_HELPER="$SCRIPT_DIR/tools/bench_helper.js"

time_command_json() {
    local cmd_json="$1"
    local runs="$2"
    local mode="${3:-self}"
    "$BRONZE_BIN" run "$BENCH_HELPER" -- time "$cmd_json" "$runs" "$mode"
}

# --- Profile run helper ---
profile_command_json() {
    local cmd_json="$1"
    "$BRONZE_BIN" run "$BENCH_HELPER" -- profile "$cmd_json"
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
    echo "Mode       : Harvest-guided AOT"
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

        exe_aot="$SCRIPT_DIR/${bench_file%.js}_aot.exe"
        exe_harvest="$SCRIPT_DIR/${bench_file%.js}_harvest.exe"
        tfb_harvest="$SCRIPT_DIR/${bench_file%.js}_harvest.tfb"
        win_js="$(to_win_path "$js_path")"
        win_exe_aot="$(to_win_path "$exe_aot")"
        win_exe_harvest="$(to_win_path "$exe_harvest")"
        win_tfb="$(to_win_path "$tfb_harvest")"

        # Compile with harvest-guided AOT (--harvest-out -> run -> --feedback-in)
        if [[ $USE_CACHED -eq 0 || ! -f "$exe_aot" || "$js_path" -nt "$exe_aot" || "$BRONZE_BIN" -nt "$exe_aot" ]]; then
            "$BRONZE_BIN" build "$win_js" -o "$win_exe_harvest" --harvest-out "$win_tfb" >/dev/null 2>&1
            if [[ $? -ne 0 || ! -f "$exe_harvest" ]]; then
                echo "Error: Failed to build $bench_file (harvest mode)" >&2
                rm -f "$exe_harvest" "${exe_harvest%.exe}.dll" "${exe_harvest%.exe}.so" "${exe_harvest%.exe}.dylib" "${exe_harvest%.exe}.pdb" "${exe_harvest%.exe}.obj" "${exe_harvest%.exe}.o" "$tfb_harvest"
                continue
            fi
            "$exe_harvest" >/dev/null 2>&1 || true
            rm -f "$exe_harvest" "${exe_harvest%.exe}.dll" "${exe_harvest%.exe}.so" "${exe_harvest%.exe}.dylib" "${exe_harvest%.exe}.pdb" "${exe_harvest%.exe}.obj" "${exe_harvest%.exe}.o"
            if [[ ! -f "$tfb_harvest" ]]; then
                echo "Error: harvest run did not produce $tfb_harvest" >&2
                continue
            fi
            "$BRONZE_BIN" build "$win_js" -o "$win_exe_aot" --feedback-in "$win_tfb" >/dev/null 2>&1
            BUILD_STATUS_AOT=$?
            [[ $USE_CACHED -eq 0 ]] && rm -f "$tfb_harvest"
            if [[ $BUILD_STATUS_AOT -ne 0 || ! -f "$exe_aot" ]]; then
                echo "Error: Failed to build $bench_file (AOT mode)" >&2
                continue
            fi
        fi

        # Time AOT executable
        stats_aot="{}"
        profile_aot="{}"
        if [[ -f "$exe_aot" ]]; then
            cmd_aot_json="$("$BRONZE_BIN" run "$BENCH_HELPER" -- json-argv "$exe_aot")"
            stats_aot="$(time_command_json "$cmd_aot_json" "$RUNS")"
            if [[ $DO_PROFILE -eq 1 ]]; then
                profile_aot="$(profile_command_json "$cmd_aot_json")"
            fi
        fi

        # Clean up if caching not requested
        if [[ $USE_CACHED -eq 0 ]]; then
            rm -f "${exe_aot%.exe}"* "$tfb_harvest"
        fi

        # Parse stats and combine with Node baseline
        record_json="$("$BRONZE_BIN" run "$BENCH_HELPER" -- record-pure "$bench_file" "$bench_desc" "$stats_aot" "$BUILD_TYPE" "$BASELINES_FILE" "$profile_aot")"

        echo "$record_json" >> "$JSONL_OUT"

        if [[ $JSON_ONLY -eq 1 ]]; then
            echo "$record_json"
        fi
    done
    if [[ $USE_CACHED -eq 0 ]]; then
        rm -f "$SCRIPT_DIR/libbronze_runtime_shared.so" "$SCRIPT_DIR/libbronze_runtime_shared.dylib" "$SCRIPT_DIR/bronze_runtime_shared.dll"
    fi
fi

# --- Execute Render Benchmarks (Bro Host) ---
if [[ $PURE_ONLY -eq 0 && -n "$BRO_PATH" ]]; then
    # 1. Bro Bronze Host SceneGraph (compiled)
    if [[ -f "$BRO_HOST_SCENEGRAPH" && ( -z "$FILTER" || "render_scenegraph_host" =~ $FILTER || "scenegraph" =~ $FILTER ) ]]; then
        appdir="$BRO_PATH/src/bronze_host/app/appdir"
        win_appdir="$(to_win_path "$appdir")"
        cmd_host_json="$("$BRONZE_BIN" run "$BENCH_HELPER" -- json-argv "$BRO_HOST_SCENEGRAPH" "$win_appdir" --headless --frames 30)"
        stats_render="$(time_command_json "$cmd_host_json" "$RUNS" wall)"

        record_json="$("$BRONZE_BIN" run "$BENCH_HELPER" -- record-render "render_scenegraph_host" "bro-bronze-host Three.js scenegraph (30 frames)" "render-compiled" "HOST" "$stats_render" "$BUILD_TYPE")"
        echo "$record_json" >> "$JSONL_OUT"
        if [[ $JSON_ONLY -eq 1 ]]; then
            echo "$record_json"
        fi
    fi

    # 2. Bro Bronze Host Wild Orbit (compiled)
    if [[ -f "$BRO_HOST_WILD" && ( -z "$FILTER" || "render_wild_orbit_host" =~ $FILTER || "wild" =~ $FILTER || "orbit" =~ $FILTER ) ]]; then
        appdir="$BRO_PATH/tests/bronze_host/appdir_wild"
        win_appdir="$(to_win_path "$appdir")"
        cmd_wild_json="$("$BRONZE_BIN" run "$BENCH_HELPER" -- json-argv "$BRO_HOST_WILD" "$win_appdir" --headless --frames 30)"
        stats_wild="$(time_command_json "$cmd_wild_json" "$RUNS" wall)"

        record_json="$("$BRONZE_BIN" run "$BENCH_HELPER" -- record-render "render_wild_orbit_host" "bro-bronze-host-wild full Three.js scene + textures + OrbitControls (30 frames)" "render-compiled" "HOST" "$stats_wild" "$BUILD_TYPE")"
        echo "$record_json" >> "$JSONL_OUT"
        if [[ $JSON_ONLY -eq 1 ]]; then
            echo "$record_json"
        fi
    fi

    # 3. Bro Bronze Host Instanced Mesh (compiled)
    if [[ -f "$BRO_HOST_INSTANCED" && ( -z "$FILTER" || "render_instanced_host" =~ $FILTER || "instanced" =~ $FILTER ) ]]; then
        appdir="$BRO_PATH/tests/bronze_host/appdir_instanced"
        win_appdir="$(to_win_path "$appdir")"
        cmd_inst_json="$("$BRONZE_BIN" run "$BENCH_HELPER" -- json-argv "$BRO_HOST_INSTANCED" "$win_appdir" --headless --frames 30)"
        stats_inst="$(time_command_json "$cmd_inst_json" "$RUNS" wall)"

        record_json="$("$BRONZE_BIN" run "$BENCH_HELPER" -- record-render "render_instanced_host" "bro-bronze-host-instanced 2,500 instances under load (30 frames)" "render-compiled" "HOST" "$stats_inst" "$BUILD_TYPE")"
        echo "$record_json" >> "$JSONL_OUT"
        if [[ $JSON_ONLY -eq 1 ]]; then
            echo "$record_json"
        fi
    fi

    # 4. bro-headless (the script compiled in process by bronze)
    if [[ -f "$BRO_HEADLESS" && -f "$SCRIPT_DIR/render_bench_interpreted.js" && ( -z "$FILTER" || "render_interpreted_bro" =~ $FILTER || "interpreted" =~ $FILTER ) ]]; then
        smoke_dir="$BRO_PATH/tests/_smoke_app"
        script_file="$SCRIPT_DIR/render_bench_interpreted.js"
        win_smoke="$(to_win_path "$smoke_dir")"
        win_script="$(to_win_path "$script_file")"
        cmd_interp_json="$("$BRONZE_BIN" run "$BENCH_HELPER" -- json-argv "$BRO_HEADLESS" "$win_smoke" "$win_script")"
        stats_interp="$(time_command_json "$cmd_interp_json" "$RUNS" wall)"

        record_json="$("$BRONZE_BIN" run "$BENCH_HELPER" -- record-render "render_interpreted_bro" "bro-headless 3D scene, script compiled in process by bronze (30 frames)" "render-interpreted" "INTERP" "$stats_interp" "$BUILD_TYPE")"
        echo "$record_json" >> "$JSONL_OUT"
        if [[ $JSON_ONLY -eq 1 ]]; then
            echo "$record_json"
        fi
    fi
fi

# --- Print Markdown Results Table ---
if [[ $JSON_ONLY -eq 0 ]]; then
    "$BRONZE_BIN" run "$BENCH_HELPER" -- report "$JSONL_OUT" "$DO_PROFILE"
fi
