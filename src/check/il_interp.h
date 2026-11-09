#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "check/prove_equiv.h"
#include "il/il.h"

namespace bronze::check {

struct InterpResult {
    bool completed = false;
    bool threw = false;
    bool isString = false;
    bool isUndefined = false;
    bool isBool = false;
    double returnNum = 0.0;
    int64_t returnI64 = 0;
    std::string returnStr;
    std::map<std::string, int64_t> finalHeapWrites;
    std::string failReason;
};

// Executes `fn` in `mod` concretely on `paramNames` and `argValues` (which may
// include both positional `arg_i` values and initial heap property paths such
// as `this.x` or `arg_0.0`).
InterpResult runIlFunctionConcrete(const il::Module& mod,
                                   const il::Function& fn,
                                   const std::vector<std::string>& paramNames,
                                   const std::vector<int64_t>& argValues,
                                   uint32_t maxSteps = 50000,
                                   bool floatBitArgs = false);

// Returns true if two completed concrete executions diverge in exception behavior,
// return value, or mutated post-state heap properties.
bool concreteResultsDiverge(const InterpResult& oldRun,
                            const InterpResult& newRun,
                            int64_t* outOldScalar = nullptr,
                            int64_t* outNewScalar = nullptr);

// Replays `res.counterexampleArgs` through `oldFn` and `newFn` in the concrete
// IL interpreter. If both executions complete and do NOT diverge, downgrades
// `res.status` to `EquivStatus::Unknown` with `detail = "prover/replay mismatch"`
// and returns false.
bool verifyCounterexampleWithReplay(const il::Module& oldMod,
                                    const il::Function& oldFn,
                                    const il::Module& newMod,
                                    const il::Function& newFn,
                                    FunctionEquivResult& res,
                                    bool floatBitArgs = false);

}  // namespace bronze::check
