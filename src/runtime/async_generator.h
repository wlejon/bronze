#pragma once

#include <cstdint>
#include "runtime/gc.h"
#include "runtime/value.h"

namespace bronze::runtime {

// The async generator object's internal slots: Frame is its body's brass
// coroutine frame (runtime/coro.h).
namespace AsyncGeneratorSlot {
enum : uint32_t {
    State = 0,
    Frame = 1,
    Queue = 2,
    CurrentPromise = 3,
    kCount = 4,
};
}

enum class AsyncGeneratorState : uint32_t {
    SuspendedStart = 0,
    SuspendedYield = 1,
    Executing = 2,
    AwaitingReturn = 3,
    Completed = 4,
};

void rtInstallAsyncGeneratorPrototype(Rooted<Value>& proto);

}  // namespace bronze::runtime
