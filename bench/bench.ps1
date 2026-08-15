<#
.SYNOPSIS
    Bronze Benchmark Suite Runner (PowerShell native)
.DESCRIPTION
    Deterministic benchmark runner comparing Bronze (infer & no-infer) against
    the pinned Node.js v24.2.0 reference baseline without needing bash or WSL.
.EXAMPLE
    .\bench\bench.ps1 --quick
    .\bench\bench.ps1 --filter math
    .\bench\bench.ps1 --profile
#>

param(
    [switch]$Quick,
    [switch]$InferOnly,
    [switch]$NoInferOnly,
    [switch]$Profile,
    [switch]$Cached,
    [int]$Runs = 5,
    [string]$Filter = "",
    [switch]$PureOnly,
    [switch]$RenderOnly,
    [switch]$AllowDebug,
    [switch]$Json,
    [string]$JsonlOut = ""
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RootDir = Split-Path -Parent $ScriptDir

if ($Quick) {
    $Runs = 3
    $InferOnly = $true
}

if (-not $JsonlOut) {
    $JsonlOut = Join-Path $ScriptDir "results.jsonl"
}

$BaselinesFile = Join-Path $ScriptDir "node_baselines.json"

# --- Locate Bronze CLI ---
function Find-BronzeCli {
    if ($env:BRONZE_CLI -and (Test-Path $env:BRONZE_CLI)) {
        return (Resolve-Path $env:BRONZE_CLI).Path
    }
    $candidates = @(
        (Join-Path $RootDir "build\dev\src\cli\bronze.exe"),
        (Join-Path $RootDir "build\src\cli\bronze.exe"),
        (Join-Path $RootDir "build\Release\bronze.exe")
    )
    foreach ($cand in $candidates) {
        if (Test-Path $cand) {
            return (Resolve-Path $cand).Path
        }
    }
    return $null
}

$BronzeBin = Find-BronzeCli
if (-not $BronzeBin) {
    Write-Error "Bronze CLI not found. Build it first via .\dev.cmd cmake --build --preset dev"
    exit 1
}

# --- Detect Build Type ---
$verOut = & $BronzeBin version 2>$null
$BuildType = "Release"
if ($verOut -match "Debug") {
    $BuildType = "Debug"
}

if ($BuildType -ne "Release" -and -not $AllowDebug) {
    Write-Error "$BronzeBin is a $BuildType binary. Benchmarks require a Release build for build-type truth."
    exit 1
}

# --- Pure Compute Benchmark Registry ---
$PureBenchmarks = @(
    @{ File = "three_math.js"; Desc = "Three.js Vector3/Matrix4 math loop against vendored three.js" },
    @{ File = "object_graph.js"; Desc = "Object-graph traversal, search, mutation, and cloning" },
    @{ File = "typed_array_crunch.js"; Desc = "Float64Array/Float32Array N-body physics & FFT crunch" },
    @{ File = "mesh_churn_2k.js"; Desc = "2k animated meshes scene graph updates & geometry churn" },
    @{ File = "instanced_mesh_churn.js"; Desc = "Three.js InstancedMesh 5,000 instances churn and color updates" },
    @{ File = "fib.js"; Desc = "Recursive fib(30) call overhead on dynamic function" },
    @{ File = "numeric_loop.js"; Desc = "10M-iteration float loop (proven-f64 arithmetic)" },
    @{ File = "property_access.js"; Desc = "1M iterations shape lookup & own-property IC dispatch" },
    @{ File = "proto_dispatch.js"; Desc = "3M iterations depth-3 inherited property read" },
    @{ File = "proto_dispatch_churn.js"; Desc = "3M iterations depth-3 read with object creation churn" },
    @{ File = "typed_array_loop.js"; Desc = "TypedArray element access vs plain array view" }
)

if (Test-Path $JsonlOut) {
    Remove-Item $JsonlOut -Force
}

if (-not $Json) {
    Write-Host "================================================================================"
    Write-Host "                      Bronze Compiler Benchmark Suite                           "
    Write-Host "================================================================================"
    Write-Host "Bronze CLI : $BronzeBin"
    Write-Host "Build Type : $BuildType"
    Write-Host "Runs / case: $Runs (+ 1 warmup discarded)"
    $modeStr = if ($InferOnly) { "Inference only" } elseif ($NoInferOnly) { "No-Infer only" } else { "Both (Infer & No-Infer)" }
    Write-Host "Mode       : $modeStr"
    if ($Profile) {
        Write-Host "Profiling  : Enabled (capturing ABI helpers & IC misses)"
    }
    Write-Host ""
}

# Helper to run timing in Python
function Measure-CommandStats($exePath, $numRuns) {
    $code = @"
import sys, subprocess, time, statistics, json

cmd = [sys.argv[1]]
runs = int(sys.argv[2])

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
    timings.append((t1 - t0) * 1000.0)
    last_out = p.stdout or ""
    last_err = p.stderr or ""

timings.sort()
med = statistics.median(timings)
mn = min(timings)
mx = max(timings)
mean_val = statistics.mean(timings)
stdev_val = statistics.stdev(timings) if len(timings) > 1 else 0.0

combined_lines = [line.strip() for line in (last_out + "\n" + last_err).splitlines() if line.strip()]
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
    "timings": [round(t, 2) for t in timings],
    "output": checksum_line
}))
"@
    $res = python -c $code "$exePath" "$numRuns"
    return ($res | ConvertFrom-Json)
}

function Measure-ProfileStats($exePath) {
    $code = @"
import sys, subprocess, json, os

cmd = [sys.argv[1]]
env = os.environ.copy()
env["BRONZE_PROFILE"] = "1"
env["BRONZE_IC_LOG"] = "1"
env["BRONZE_GC_LOG"] = "1"

try:
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=120, env=env)
    output = (p.stdout or "") + "\n" + (p.stderr or "")
    
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
"@
    $res = python -c $code "$exePath"
    return ($res | ConvertFrom-Json)
}

# Load Node Baselines
$NodeBaselines = @{}
if (Test-Path $BaselinesFile) {
    $rawBase = Get-Content $BaselinesFile -Raw | ConvertFrom-Json
    if ($rawBase.benchmarks) {
        $rawBase.benchmarks.PSObject.Properties | ForEach-Object {
            $NodeBaselines[$_.Name] = $_.Value
        }
    }
}

$AllRecords = @()

if (-not $RenderOnly) {
    foreach ($item in $PureBenchmarks) {
        $benchFile = $item.File
        $benchDesc = $item.Desc

        if ($Filter -and ($benchFile -notmatch $Filter) -and ($benchDesc -notmatch $Filter)) {
            continue
        }

        $jsPath = Join-Path $ScriptDir $benchFile
        if (-not (Test-Path $jsPath)) {
            continue
        }

        $baseName = [System.IO.Path]::GetFileNameWithoutExtension($benchFile)
        $exeInfer = Join-Path $ScriptDir "${baseName}_infer.exe"
        $exeNoInfer = Join-Path $ScriptDir "${baseName}_noinfer.exe"

        # 1. Compile Infer
        if (-not $NoInferOnly) {
            $needBuild = -not $Cached -or (-not (Test-Path $exeInfer)) -or `
                         ((Get-Item $jsPath).LastWriteTimeUtc -gt (Get-Item $exeInfer).LastWriteTimeUtc) -or `
                         ((Get-Item $BronzeBin).LastWriteTimeUtc -gt (Get-Item $exeInfer).LastWriteTimeUtc)
            if ($needBuild) {
                & $BronzeBin build $jsPath -o $exeInfer *> $null
                if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exeInfer)) {
                    Write-Warning "Failed to build $benchFile (infer mode)"
                    continue
                }
            }
        }

        # 2. Compile No-Infer
        if (-not $InferOnly) {
            $needBuild = -not $Cached -or (-not (Test-Path $exeNoInfer)) -or `
                         ((Get-Item $jsPath).LastWriteTimeUtc -gt (Get-Item $exeNoInfer).LastWriteTimeUtc) -or `
                         ((Get-Item $BronzeBin).LastWriteTimeUtc -gt (Get-Item $exeNoInfer).LastWriteTimeUtc)
            if ($needBuild) {
                & $BronzeBin build $jsPath -o $exeNoInfer --no-infer *> $null
                if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exeNoInfer)) {
                    Write-Warning "Failed to build $benchFile (--no-infer mode)"
                    continue
                }
            }
        }

        # Run Timings
        $statsInfer = $null
        $profInfer = $null
        if (-not $NoInferOnly -and (Test-Path $exeInfer)) {
            $statsInfer = Measure-CommandStats $exeInfer $Runs
            if ($Profile) {
                $profInfer = Measure-ProfileStats $exeInfer
            }
        }

        $statsNoInfer = $null
        if (-not $InferOnly -and (Test-Path $exeNoInfer)) {
            $statsNoInfer = Measure-CommandStats $exeNoInfer $Runs
        }

        if (-not $Cached) {
            if (Test-Path $exeInfer) { Remove-Item $exeInfer -Force }
            if (Test-Path $exeNoInfer) { Remove-Item $exeNoInfer -Force }
        }

        $nodeEntry = $NodeBaselines[$benchFile]
        $nodeMed = if ($nodeEntry) { $nodeEntry.median_ms } else { $null }

        $infMed = if ($statsInfer) { $statsInfer.median_ms } else { $null }
        $noinfMed = if ($statsNoInfer) { $statsNoInfer.median_ms } else { $null }

        $speedup = if ($infMed -and $noinfMed -and $infMed -gt 0) { [Math]::Round($noinfMed / $infMed, 2) } else { 1.0 }
        $vsNode = if ($nodeMed -and $infMed -and $infMed -gt 0) { [Math]::Round($nodeMed / $infMed, 2) } else { $null }

        $status = "N/A"
        if ($vsNode -ne $null) {
            if ($vsNode -ge 1.05) { $status = "WIN" }
            elseif ($vsNode -ge 0.95) { $status = "PARITY" }
            else { $status = "BEHIND" }
        }

        $record = @{
            name = $benchFile
            description = $benchDesc
            category = "pure-compute"
            build_type = $BuildType
            infer = $statsInfer
            noinfer = $statsNoInfer
            infer_speedup = $speedup
            node_median_ms = $nodeMed
            vs_node = $vsNode
            status = $status
            profile = $profInfer
            output_match = if ($statsInfer -and $statsNoInfer) { $statsInfer.output -eq $statsNoInfer.output } else { $true }
        }

        $AllRecords += $record
        $recordJson = $record | ConvertTo-Json -Compress -Depth 5
        Add-Content -Path $JsonlOut -Value $recordJson

        if ($Json) {
            Write-Output $recordJson
        }
    }
}

# --- Print Results Table ---
if (-not $Json) {
    Write-Host ""
    Write-Host "### Pure-Compute Benchmarks (Compiled Native vs Node.js Baseline)"
    Write-Host "| Benchmark | Inferred (ms) | Node.js (ms) | vs Node | Status | No-Infer (ms) | Speedup (Infer) | Checksum / Output |"
    Write-Host "|---|---|---|---|---|---|---|---|"

    foreach ($r in $AllRecords) {
        if ($r.category -ne "pure-compute") { continue }
        $name = $r.name
        $infMed = if ($r.infer) { "{0:N2}" -f $r.infer.median_ms } else { "-" }
        $noinfMed = if ($r.noinfer) { "{0:N2}" -f $r.noinfer.median_ms } else { "-" }
        $speedup = if ($r.infer -and $r.noinfer) { "{0:N2}x" -f $r.infer_speedup } else { "-" }
        $nodeMed = if ($r.node_median_ms) { "{0:N2}" -f $r.node_median_ms } else { "-" }
        $vsNode = if ($r.vs_node) { "{0:N2}x" -f $r.vs_node } else { "-" }
        
        $badge = switch ($r.status) {
            "WIN" { "**WIN**" }
            "PARITY" { "PARITY" }
            "BEHIND" { "BEHIND" }
            default { "-" }
        }

        $out = if ($r.infer) { $r.infer.output } elseif ($r.noinfer) { $r.noinfer.output } else { "" }
        Write-Host "| ``$name`` | **$infMed** | $nodeMed | **$vsNode** | $badge | $noinfMed | $speedup | ``$out`` |"
    }

    if ($Profile) {
        Write-Host ""
        Write-Host "#### Profile Miss Breakdown (--profile)"
        Write-Host "| Benchmark | Total Helpers | Top ABI Helpers | Top IC Miss Reason |"
        Write-Host "|---|---|---|---|"
        foreach ($r in $AllRecords) {
            if ($r.category -ne "pure-compute" -or -not $r.profile) { continue }
            $name = $r.name
            $tot = "{0:N0}" -f $r.profile.total_helpers
            $helpers = if ($r.profile.top_helpers) {
                ($r.profile.top_helpers | ForEach-Object { "$($_.name): $($_.count)" }) -join ", "
            } else { "None" }
            $topMiss = if ($r.profile.top_miss) { $r.profile.top_miss } else { "None" }
            Write-Host "| ``$name`` | $tot | $helpers | $topMiss |"
        }
    }

    Write-Host ""
    Write-Host "Machine-readable results saved to '$JsonlOut'."
}
