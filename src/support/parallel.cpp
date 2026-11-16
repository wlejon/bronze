#include "support/parallel.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <numeric>
#include <thread>

namespace bronze {

size_t threadsForWork(size_t requested, size_t items, size_t totalWeight, size_t minWeightPerThread) {
    size_t threads = requested != 0 ? requested : std::max<size_t>(1, std::thread::hardware_concurrency());
    if (minWeightPerThread != 0) threads = std::min(threads, std::max<size_t>(1, totalWeight / minWeightPerThread));
    return std::max<size_t>(1, std::min(threads, items));
}

std::vector<size_t> heaviestFirst(const std::vector<size_t>& weights) {
    std::vector<size_t> order(weights.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return weights[a] > weights[b]; });
    return order;
}

void parallelForEach(const std::vector<size_t>& order, size_t threads, const std::function<void(size_t)>& body) {
    if (threads <= 1 || order.size() <= 1) {
        for (size_t i : order) body(i);
        return;
    }
    std::atomic<size_t> next{0};
    std::vector<std::exception_ptr> errors(order.size());
    auto worker = [&] {
        for (size_t k = next.fetch_add(1); k < order.size(); k = next.fetch_add(1)) {
            try {
                body(order[k]);
            } catch (...) {
                errors[k] = std::current_exception();
            }
        }
    };
    std::vector<std::thread> pool;
    const size_t extra = std::min(threads, order.size()) - 1;
    pool.reserve(extra);
    for (size_t t = 0; t < extra; ++t) pool.emplace_back(worker);
    worker();
    for (std::thread& t : pool) t.join();
    size_t first = order.size();
    for (size_t k = 0; k < order.size(); ++k) {
        if (errors[k] && (first == order.size() || order[k] < order[first])) first = k;
    }
    if (first != order.size()) std::rethrow_exception(errors[first]);
}

TaskPool::TaskPool(size_t threads) {
    if (threads <= 1) return;
    threads_.reserve(threads);
    for (size_t t = 0; t < threads; ++t) threads_.emplace_back([this] { work(); });
}

TaskPool::~TaskPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();
    for (std::thread& t : threads_) t.join();
}

void TaskPool::submit(std::function<void()> task) {
    if (threads_.empty()) {
        task();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(task));
    }
    ready_.notify_one();
}

void TaskPool::work() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) return;
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        task();
    }
}

}  // namespace bronze
