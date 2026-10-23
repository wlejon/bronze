#pragma once

#include <brass/core/diagnostics.hpp>
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>
#include <string>
#include <vector>

namespace il2mir {

struct BridgeVerifyError {
    std::string function_name;
    uint32_t block_id = 0;
    std::string message;
};

struct BridgeVerifyResult {
    bool ok = true;
    std::vector<BridgeVerifyError> errors;
};

// Verify Bronze IL -> Brass MIR bridge invariants on a single function or
// module:
//  1. Structural MIR verification (brass::verify_function / verify_module).
//  2. NaN-box canonicalization invariant: every bitcast_i64_f64 of a general
//     f64 must either feed a sign-masked range check (& 0x7FFFFFFFFFFFFFFF) or
//     be gated by the canonical-NaN select (ugt(abs, 0x7FF0000000000000) ?
//     0x7FF8000000000000 : bits) so negative NaNs (0xFFF8...) never leak into
//     the NaN-box tag space (> 0xFFF0000000000000).
//  3. Unsigned NaN-box tag comparison invariant: ordering comparisons against
//     NaN-box tag boundaries (0xFFF0000000000000, 0x7FF0000000000000,
//     0x43E0000000000000, etc.) must use unsigned predicates (ult/ule/ugt/uge),
//     never signed predicates (slt/sle/sgt/sge).
//  4. Tag-mask comparison invariant: (v & 0xFFFF000000000000) == tag must
//     compare against a valid Bronze shifted tag constant.
BridgeVerifyResult verify_bridge_function_detailed(const brass::Function& fn);
BridgeVerifyResult verify_bridge_module_detailed(const brass::Module& mod);

bool verify_bridge_function(const brass::Function& fn, brass::DiagnosticReporter* diag = nullptr);
bool verify_bridge_module(const brass::Module& mod, brass::DiagnosticReporter* diag = nullptr);

} // namespace il2mir
