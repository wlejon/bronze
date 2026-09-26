#pragma once

#include <cstdint>

#include "abi/bronze_abi.h"
#include "runtime/gc.h"
#include "runtime/value.h"

namespace bronze::runtime {

// A generator object's INTERNAL SLOTS (ECMA-262 27.5.1.1), named after the
// spec's. Real fields on the object, like every other iterator kind's, which is
// what makes them invisible to `Object.getOwnPropertyNames` as well as to
// `Object.keys` — and what makes a generator object's own-key list empty, since
// `next` is inherited from %GeneratorPrototype%.
//
// [[GeneratorContext]] is the body's brass coroutine FRAME (runtime/coro.h),
// held as an object reference so the collector traces it through the
// generator object.
namespace GeneratorSlot {
enum : uint32_t { State, Frame, kCount };
}

// [[GeneratorState]] (27.5.1.1). `executing` is the one that exists only while
// the body is on the stack, and the only reason it is a stored state rather
// than an inferred one: 27.5.3.2 step 2 has to answer "is this generator
// already running" from a call made INSIDE the body.
namespace GeneratorState {
enum : uint32_t { SuspendedStart, SuspendedYield, Executing, Completed };
}

// Which of the three methods is resuming: brass's CoroResumeMode, which the
// compiled body reads after each suspension (bronze_abi.h).
namespace GeneratorResumeMode {
enum : uint32_t {
    Next = BRONZE_ABI_RESUME_NEXT,
    Throw = BRONZE_ABI_RESUME_THROW,
    Return = BRONZE_ABI_RESUME_RETURN,
};
}

// `next`, `return` and `throw` onto %GeneratorPrototype%. Called once, while
// that prototype is being built (runtime/iterator.cpp), because the prototype
// is the only place 27.5.1 puts them.
void rtInstallGeneratorPrototype(Rooted<Value>& proto);

// A generator object over `frame`, a body that has bound its arguments and
// stopped at its START suspension (runtime/coro.cpp).
Value rtCreateGeneratorObject(Rooted<Value>& frame);

}  // namespace bronze::runtime
