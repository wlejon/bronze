#pragma once

// Best-effort native symbolization for the runtime's own diagnostics — the
// sampling profiler (sampler.cpp) and the shape census (shape_census.cpp)
// both turn raw code addresses into `module!Function` at DUMP time, and this
// is the one implementation they share so the two reports can never disagree
// about what a PC is called.
//
// dbghelp, lazily initialized on first use, never on a hot path: every caller
// is an atexit dump walking a few thousand unique addresses once. On
// non-Windows platforms everything here degrades to hex addresses; the
// instruments still run, they just report addresses.

#include <cstddef>
#include <cstdint>

namespace bronze::runtime {

struct SymbolizedPc {
    // Function name as the PDB spells it (a compiled JS function keeps its IL
    // name verbatim: `Matrix4.multiplyMatrices`, `__anon_fn_1354`,
    // `main.seg3`). Empty when no symbol resolved.
    char name[256];
    // Basename of the module the address is inside ("app.dll",
    // "bronze_runtime_shared.dll", "bro-headless.exe"), or "?" when the
    // address is in no loaded module.
    char module[64];
    // Start address of the containing function when the symbol resolved —
    // the aggregation key that makes two PCs inside one function one row —
    // else the PC rounded down to 16 bytes so unresolved addresses still
    // bucket stably.
    uint64_t funcStart;
    bool resolved;
};

// Symbolizes one address. Thread-unsafe by design (dbghelp is); call from a
// single dump path only.
void symbolizePc(uint64_t pc, SymbolizedPc& out);

}  // namespace bronze::runtime
