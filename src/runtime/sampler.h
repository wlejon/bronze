#pragma once

// In-process sampling profiler for bronze-compiled code (BRONZE_SAMPLE=1).
//
// A runtime-owned sampler thread suspends the JS thread on a fixed period,
// reads its context, walks the stack through the modules' own x64 unwind
// data, and appends raw PCs to a preallocated log. Nothing is symbolized and
// nothing allocates while the target is suspended; the whole translation to
// `module!Function` happens once, at exit, through symbolize.h against the
// PDBs bronze already links beside every module (src/cli/link.cpp). Compiled
// JS functions therefore report under their IL names —
// `Matrix4.multiplyMatrices`, `main.seg3` — at function granularity, which is
// the ceiling bronze's debug info supports (no line tables are emitted).
//
// Zero overhead when BRONZE_SAMPLE is unset: the one entry point below is a
// static-bool test called once per module entry, no codegen change, no new
// ABI surface.
//
// Env:
//   BRONZE_SAMPLE=1          enable (Windows only; elsewhere a no-op)
//   BRONZE_SAMPLE_HZ=N       sample rate, default 1000, clamped to [50, 4000]
//   BRONZE_SAMPLE_OUT=path   JSON report path, default "bronze_sample.json"
//   BRONZE_SAMPLE_TAIL_MS=N  also emit a table restricted to the last N ms of
//                            samples — the steady-state window of a bench run
//                            whose startup would otherwise pollute the bill
//
// Prior art and the failure modes this design answers are documented in
// D:/projects/brobench/analysis/chunk4_native_bill.md (winsample.cpp): parked-
// thread false attribution does not arise because the one sampled thread is
// the JS thread by construction; Sleep(1)=Sleep(15.6) is answered with
// timeBeginPeriod(1); symbol-cache staleness does not arise because
// symbolization happens once, at exit, in-process.

namespace bronze::runtime {

// Declares the CALLING thread to be the thread that runs compiled JS, and —
// on the first call with BRONZE_SAMPLE=1 — starts the sampler against it.
// Called from embed::runMain and embed::runEntry, which are the two places a
// compiled module's code enters the process. Subsequent calls (a host running
// several modules) are no-ops.
void samplerNoteJsThread() noexcept;

}  // namespace bronze::runtime
