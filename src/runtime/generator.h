#pragma once

#include <cstdint>

#include "runtime/gc.h"
#include "runtime/value.h"

namespace bronze::runtime {

// A generator object's INTERNAL SLOTS (ECMA-262 27.5.1.1), named after the
// spec's. Real fields on the object, like every other iterator kind's, which is
// what makes them invisible to `Object.getOwnPropertyNames` as well as to
// `Object.keys` — and what makes a generator object's own-key list empty, since
// `next` is inherited from %GeneratorPrototype%.
//
// [[GeneratorContext]] is a CLOSURE here rather than a saved execution context:
// bronze compiles a generator body into a resume function over a frame record,
// so "the context to resume" is exactly that function value.
namespace GeneratorSlot {
enum : uint32_t { State, Resume, kCount };
}

// [[GeneratorState]] (27.5.1.1). `executing` is the one that exists only while
// the body is on the stack, and the only reason it is a stored state rather
// than an inferred one: 27.5.3.2 step 2 has to answer "is this generator
// already running" from a call made INSIDE the body.
namespace GeneratorState {
enum : uint32_t { SuspendedStart, SuspendedYield, Executing, Completed };
}

// Which of the three methods is resuming, as the resume function's first
// argument. Pinned against src/lower/lower_generator.cpp, which compiles the
// dispatch that reads it.
namespace GeneratorResumeMode {
enum : uint32_t { Next, Return, Throw };
}

// `next`, `return` and `throw` onto %GeneratorPrototype%. Called once, while
// that prototype is being built (runtime/iterator.cpp), because the prototype
// is the only place 27.5.1 puts them.
void rtInstallGeneratorPrototype(Rooted<Value>& proto);

}  // namespace bronze::runtime
