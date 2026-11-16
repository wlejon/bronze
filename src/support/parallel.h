#pragma once

// Fork-join over a fixed set of independent work items, for the optimiser's
// per-function passes (opt/redundancy's module walk).
//
// The contract that keeps the result the same on any number of threads is
// the caller's: an item reads nothing another item writes, and writes only
// what it owns (its function, its own result slot). Under it, which thread
// runs an item and when cannot show, and one thread runs every item on the
// calling thread, in order, through the same code. (brass/core/parallel_for
// is the same helper for the backend; the optimiser does not link brass.)

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace bronze {

// The threads a fan-out over `totalWeight` units of work uses when
// `requested` are allowed (0: the machine's hardware threads): 1 below
// `minWeightPerThread` units, never more than `items`.
size_t threadsForWork(size_t requested, size_t items, size_t totalWeight, size_t minWeightPerThread);

// The indices 0..weights.size()-1, heaviest first, ties by index: the order
// to start items in (the last item to start bounds the wall time).
std::vector<size_t> heaviestFirst(const std::vector<size_t>& weights);

// Runs body(order[0]), body(order[1]), ... each exactly once on up to
// `threads` threads, the calling thread one of them; returns when all have
// run. An item that throws does not stop the others; once all have run, the
// exception of the item with the smallest value is rethrown.
void parallelForEach(const std::vector<size_t>& order, size_t threads, const std::function<void(size_t)>& body);

// Up to `threads` threads running tasks as they are submitted, in submission
// order; the destructor runs what is queued and joins. With `threads` <= 1 a
// task runs in submit(), on the calling thread. A task must not throw (hand
// its exception over, through a std::promise say).
class TaskPool {
public:
    explicit TaskPool(size_t threads);
    ~TaskPool();
    TaskPool(const TaskPool&) = delete;
    TaskPool& operator=(const TaskPool&) = delete;

    void submit(std::function<void()> task);

private:
    void work();

    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> queue_;
    bool stopping_ = false;
    std::vector<std::thread> threads_;
};

}  // namespace bronze
