#pragma once

// TEST FOLDING (job 2): a test whose answer the facts already hold becomes
// that answer. This is what deletes the second and every later guard on the
// same value, and, once the branch on it becomes a jump, the guard's whole
// exit block.
//
// Answers come from three places, each exact:
//   - the FactEnv at the test's position (opt/core fact_env.h), which holds
//     what dominating branch edges proved (and what a loop index's `i < n`
//     proves, index_facts.h) and has already dropped memory facts killed
//     since;
//   - the test's subject's own definition (a `box` of a typed value is never
//     an object, a hole or nullish; a fresh allocation is an object);
//   - for `ptr.eq (load.shape x), (shape.cell n)`, the shape fact about x
//     PROVIDED the load is still the available read of x's shape in the
//     table, i.e. no write of `Shapes` lies between the load and here — the
//     fact describes the present, and the load must read the present.

#include <optional>

#include "il/il.h"
#include "opt/core/fact_env.h"
#include "opt/redundancy/expr_table.h"
#include "opt/redundancy/fold.h"

namespace bronze::opt::redundancy {

// The value of test `inst` (operands resolved) if it is known here, else
// nullopt. Does not modify anything.
// The operand `construct.result` answers with when the facts say whether
// the constructor's value is an object; kNoValue when they do not.
ValueId knownConstructResult(const il::Instruction& inst, const FactEnv& facts, const ValueView& values);

std::optional<bool> knownTestResult(const il::Instruction& inst, const FactEnv& facts,
                                    const ExprTable& exprs, const ValueView& values);

// `shape.has_own (load.shape x), <slot>, "k"` answered by the shape fact
// about x (a dominating `ptr.eq` against a shape cell), when the load still
// reads the present shape: the cell's name places the key, so the test is
// whether it places it at that slot with the attributes the access needs.
// The write form is answered as the cell's own write chain is (prop_write.cpp:
// a resolved cell's shape was no prototype's when it resolved).
std::optional<bool> knownHasOwnResult(const il::Instruction& inst, const FactEnv& facts, const ExprTable& exprs,
                                      const ValueView& values, const il::Module& module);

// Whether `x` is certainly a Number here: its definition boxes one (or reads
// a Number form), or a dominating test proved it.
bool knownNumber(const FactEnv& facts, const ValueView& values, ValueId x);

}  // namespace bronze::opt::redundancy
