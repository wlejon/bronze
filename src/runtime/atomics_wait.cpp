#include "runtime/atomics_wait.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "runtime/bigint.h"
#include "runtime/builtin_typed_array_internal.h"
#include "runtime/compartment.h"
#include "runtime/exception.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/host_interrupt.h"
#include "runtime/object.h"
#include "runtime/promise.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shared_store.h"
#include "runtime/typed_array.h"

namespace bronze::runtime {

namespace {

using Clock = std::chrono::steady_clock;

template <typename T>
bool isAligned(const void* ptr) noexcept {
    return (reinterpret_cast<uintptr_t>(ptr) % std::atomic_ref<T>::required_alignment) == 0;
}

std::mutex& fallbackAtomicMutex() {
    static auto* mu = new std::mutex();
    return *mu;
}

int32_t toInt32(Value v) {
    const double n = rtToNumber(v);
    if (!std::isfinite(n) || n == 0.0) return 0;
    constexpr double kTwo32 = 4294967296.0;
    constexpr double kTwo31 = 2147483648.0;
    double m = std::fmod(std::trunc(n), kTwo32);
    if (m < 0.0) m += kTwo32;
    if (m >= kTwo31) return static_cast<int32_t>(static_cast<int64_t>(m - kTwo32));
    return static_cast<int32_t>(static_cast<uint32_t>(m));
}

bool parseTimeout(Value v, bool& hasDeadline, double& timeoutMs) {
    const double q = rtToNumber(v);
    if (std::isnan(q)) {
        hasDeadline = false;
        timeoutMs = 0.0;
        return true;
    }
    const double t = q <= 0.0 ? 0.0 : q;
    if (!std::isfinite(t) || t > 1e12) {
        hasDeadline = false;
        timeoutMs = 0.0;
        return true;
    }
    hasDeadline = true;
    timeoutMs = t;
    return true;
}

uint64_t maskWidth(uint32_t width) noexcept {
    return width == 8 ? ~uint64_t{0} : ((uint64_t{1} << (width * 8)) - 1);
}

enum class WaiterKind : uint8_t { Sync, Async };

struct WaiterNode {
    WaiterKind kind = WaiterKind::Sync;
    const void* addr = nullptr;
    SharedBufferBlock* block = nullptr;
    bool notified = false;
    bool removed = false;
    std::condition_variable cv;
    bool hasDeadline = false;
    Clock::time_point deadline{};

    ~WaiterNode() {
        if (block) rtReleaseSharedStore(block);
    }
};

struct WaitTable {
    std::mutex mu;
    std::unordered_map<const void*, std::deque<std::shared_ptr<WaiterNode>>> queues;
};

WaitTable& waitTable() {
    static auto* table = new WaitTable();
    return *table;
}

void removeWaiterLocked(WaitTable& wt, const std::shared_ptr<WaiterNode>& node) {
    if (node->removed) return;
    node->removed = true;
    auto it = wt.queues.find(node->addr);
    if (it == wt.queues.end()) return;
    auto& q = it->second;
    for (auto qit = q.begin(); qit != q.end(); ++qit) {
        if (*qit == node) {
            q.erase(qit);
            break;
        }
    }
    if (q.empty()) wt.queues.erase(it);
}

struct AsyncEntry {
    Value promise = Value::fromUndefined();
    std::shared_ptr<WaiterNode> node;
};

thread_local std::vector<AsyncEntry> t_asyncWaiters;
thread_local std::vector<std::pair<Value, bool>> t_settlingWaiters;
thread_local bool t_asyncRootsRegistered = false;

void ensureAsyncWaiterRoots() {
    if (t_asyncRootsRegistered) return;
    t_asyncRootsRegistered = true;
    rtHeap().add_root_source([](const Heap::RootVisitor& visit) {
        for (AsyncEntry& entry : t_asyncWaiters) visit(entry.promise);
        for (auto& s : t_settlingWaiters) visit(s.first);
    });
}

struct SharedBlockRetainer {
    SharedBufferBlock* block = nullptr;
    ~SharedBlockRetainer() { rtReleaseSharedStore(block); }
};

void syncGrowableSharedLength(ArrayBufferHeader* buf, uint64_t bytesAddr) {
    if (!buf->isResizable()) return;
    if (SharedBufferBlock* block = rtFindSharedStore(bytesAddr)) {
        const uint32_t len = block->byteLength.load(std::memory_order_acquire);
        rtReleaseSharedStore(block);
        if (len != buf->byteLength) {
            buf->byteLength = len;
            Rooted<Value> bufRoot{Value::fromObject(buf)};
            closeOrReopenViews(rtHeap(), bufRoot);
        }
    }
}

struct WaitTarget {
    bool isBigInt = false;
    uint32_t index = 0;
    int64_t expected = 0;
    bool hasDeadline = false;
    double timeoutMs = 0.0;
};

bool validateWaitable(const char* method, Rooted<Value>& view, Value indexVal, Value expectedVal,
                      Value timeoutVal, bool requireShared, bool withValueAndTimeout, WaitTarget& out) {
    if (!typed_array_internal::isTypedArray(view.get())) {
        rtThrowTypeError(std::string("Atomics.") + method +
                         " called on a value that is not a waitable typed array");
        return false;
    }
    auto* ta = view.get().asObject<TypedArrayHeader>();
    const ElementKind ek = ta->elementKind();
    if (ek != ElementKind::Int32 && ek != ElementKind::BigInt64) {
        rtThrowTypeError(std::string("Atomics.") + method + " requires an Int32Array or BigInt64Array");
        return false;
    }
    out.isBigInt = (ek == ElementKind::BigInt64);
    if (!ta->buffer.isObject()) {
        rtThrowTypeError("TypedArray has no ArrayBuffer");
        return false;
    }
    auto* buf = ta->buffer.asObject<ArrayBufferHeader>();
    if (requireShared && !buf->isShared()) {
        rtThrowTypeError(std::string("Atomics.") + method + " requires a SharedArrayBuffer");
        return false;
    }
    if (buf->isDetached()) {
        rtThrowTypeError("ArrayBuffer is detached");
        return false;
    }
    if (buf->isShared()) {
        syncGrowableSharedLength(buf, buf->externalPtrBits);
        ta = view.get().asObject<TypedArrayHeader>();
    }
    Rooted<Value> indexRoot{indexVal};
    if (!typed_array_internal::toIndex(indexRoot.get(), "index", 1, out.index)) return false;
    if (out.index >= view.get().asObject<TypedArrayHeader>()->length) {
        rtThrowRangeError("Invalid atomic access index");
        return false;
    }
    if (withValueAndTimeout) {
        Rooted<Value> expRoot{expectedVal};
        if (out.isBigInt) {
            uint64_t raw = 0;
            if (!rtBigIntToRawBits64(expRoot.get(), raw)) return false;
            out.expected = static_cast<int64_t>(raw);
        } else {
            out.expected = static_cast<int64_t>(toInt32(expRoot.get()));
        }
        if (!parseTimeout(timeoutVal, out.hasDeadline, out.timeoutMs)) return false;
    }
    return true;
}

Value makeWaitAsyncResult(bool isAsync, Value innerValue) {
    Rooted<Value> valRoot{innerValue};
    Rooted<Value> obj{Value::fromObject(ObjectHeader::create(rtHeap(), rtArena(), rtPlainObjectShape()))};
    Rooted<Value> asyncKey{rtMakeString("async")};
    Rooted<Value> asyncVal{Value::fromBool(isAsync)};
    obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), asyncKey, asyncVal, nullptr,
                                                /*enumerable=*/true, /*defineOwn=*/true);
    Rooted<Value> valueKey{rtMakeString("value")};
    obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), valueKey, valRoot, nullptr,
                                                /*enumerable=*/true, /*defineOwn=*/true);
    return obj.get();
}

int64_t loadSigned(const void* addr, bool isBigInt) noexcept {
    if (isBigInt) {
        const auto* p = static_cast<const uint8_t*>(addr);
        return static_cast<int64_t>(rtAtomicLoadBytes(p, 8));
    }
    const auto* p = static_cast<const uint8_t*>(addr);
    return static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(rtAtomicLoadBytes(p, 4))));
}

}  // namespace

uint64_t rtAtomicLoadBytes(const uint8_t* ptr, uint32_t width) noexcept {
    switch (width) {
        case 1:
            return std::atomic_ref<const uint8_t>(*ptr).load(std::memory_order_seq_cst);
        case 2:
            if (isAligned<uint16_t>(ptr)) {
                return std::atomic_ref<const uint16_t>(*reinterpret_cast<const uint16_t*>(ptr))
                    .load(std::memory_order_seq_cst);
            }
            break;
        case 4:
            if (isAligned<uint32_t>(ptr)) {
                return std::atomic_ref<const uint32_t>(*reinterpret_cast<const uint32_t*>(ptr))
                    .load(std::memory_order_seq_cst);
            }
            break;
        case 8:
            if (isAligned<uint64_t>(ptr)) {
                return std::atomic_ref<const uint64_t>(*reinterpret_cast<const uint64_t*>(ptr))
                    .load(std::memory_order_seq_cst);
            }
            break;
        default:
            break;
    }
    std::lock_guard<std::mutex> lock(fallbackAtomicMutex());
    uint64_t bits = 0;
    std::memcpy(&bits, ptr, width);
    return bits;
}

void rtAtomicStoreBytes(uint8_t* ptr, uint32_t width, uint64_t value) noexcept {
    switch (width) {
        case 1:
            std::atomic_ref<uint8_t>(*ptr).store(static_cast<uint8_t>(value), std::memory_order_seq_cst);
            return;
        case 2:
            if (isAligned<uint16_t>(ptr)) {
                std::atomic_ref<uint16_t>(*reinterpret_cast<uint16_t*>(ptr))
                    .store(static_cast<uint16_t>(value), std::memory_order_seq_cst);
                return;
            }
            break;
        case 4:
            if (isAligned<uint32_t>(ptr)) {
                std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(ptr))
                    .store(static_cast<uint32_t>(value), std::memory_order_seq_cst);
                return;
            }
            break;
        case 8:
            if (isAligned<uint64_t>(ptr)) {
                std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(ptr))
                    .store(value, std::memory_order_seq_cst);
                return;
            }
            break;
        default:
            break;
    }
    std::lock_guard<std::mutex> lock(fallbackAtomicMutex());
    std::memcpy(ptr, &value, width);
}

namespace {

template <typename T>
uint64_t atomicRmwTyped(T* ptr, int op, uint64_t operand) noexcept {
    std::atomic_ref<T> ref(*ptr);
    const T v = static_cast<T>(operand);
    switch (op) {
        case 0: return ref.fetch_add(v, std::memory_order_seq_cst);
        case 1: return ref.fetch_sub(v, std::memory_order_seq_cst);
        case 2: return ref.fetch_and(v, std::memory_order_seq_cst);
        case 3: return ref.fetch_or(v, std::memory_order_seq_cst);
        case 4: return ref.fetch_xor(v, std::memory_order_seq_cst);
        case 5: return ref.exchange(v, std::memory_order_seq_cst);
        default: return 0;
    }
}

}  // namespace

uint64_t rtAtomicRmwBytes(uint8_t* ptr, uint32_t width, int op, uint64_t operand) noexcept {
    switch (width) {
        case 1:
            return atomicRmwTyped(ptr, op, operand);
        case 2:
            if (isAligned<uint16_t>(ptr)) return atomicRmwTyped(reinterpret_cast<uint16_t*>(ptr), op, operand);
            break;
        case 4:
            if (isAligned<uint32_t>(ptr)) return atomicRmwTyped(reinterpret_cast<uint32_t*>(ptr), op, operand);
            break;
        case 8:
            if (isAligned<uint64_t>(ptr)) return atomicRmwTyped(reinterpret_cast<uint64_t*>(ptr), op, operand);
            break;
        default:
            break;
    }
    std::lock_guard<std::mutex> lock(fallbackAtomicMutex());
    const uint64_t mask = maskWidth(width);
    uint64_t oldBits = 0;
    std::memcpy(&oldBits, ptr, width);
    uint64_t next = oldBits;
    switch (op) {
        case 0: next = (oldBits + operand) & mask; break;
        case 1: next = (oldBits - operand) & mask; break;
        case 2: next = (oldBits & operand) & mask; break;
        case 3: next = (oldBits | operand) & mask; break;
        case 4: next = (oldBits ^ operand) & mask; break;
        case 5: next = operand & mask; break;
        default: break;
    }
    std::memcpy(ptr, &next, width);
    return oldBits;
}

uint64_t rtAtomicCompareExchangeBytes(uint8_t* ptr, uint32_t width, uint64_t expected,
                                      uint64_t replacement) noexcept {
    auto cas = [&](auto* typedPtr) -> uint64_t {
        using T = std::remove_pointer_t<decltype(typedPtr)>;
        T exp = static_cast<T>(expected);
        const T rep = static_cast<T>(replacement);
        std::atomic_ref<T>(*typedPtr).compare_exchange_strong(exp, rep, std::memory_order_seq_cst);
        return exp;
    };
    switch (width) {
        case 1: return cas(ptr);
        case 2: if (isAligned<uint16_t>(ptr)) return cas(reinterpret_cast<uint16_t*>(ptr)); break;
        case 4: if (isAligned<uint32_t>(ptr)) return cas(reinterpret_cast<uint32_t*>(ptr)); break;
        case 8: if (isAligned<uint64_t>(ptr)) return cas(reinterpret_cast<uint64_t*>(ptr)); break;
        default: break;
    }
    std::lock_guard<std::mutex> lock(fallbackAtomicMutex());
    const uint64_t mask = maskWidth(width);
    uint64_t oldBits = 0;
    std::memcpy(&oldBits, ptr, width);
    if (oldBits == (expected & mask)) {
        const uint64_t next = replacement & mask;
        std::memcpy(ptr, &next, width);
    }
    return oldBits;
}

Value rtAtomicsWait(Value viewVal, Value indexVal, Value expectedVal, Value timeoutVal) {
    Rooted<Value> view{viewVal};
    WaitTarget target;
    if (!validateWaitable("wait", view, indexVal, expectedVal, timeoutVal, /*requireShared=*/true,
                          /*withValueAndTimeout=*/true, target)) {
        return Value::fromUndefined();
    }
    auto* ta = view.get().asObject<TypedArrayHeader>();
    if (target.index >= ta->length) return rtThrowRangeError("Invalid atomic access index");
    auto* buf = ta->buffer.asObject<ArrayBufferHeader>();
    const uint32_t width = target.isBigInt ? 8u : 4u;
    const void* addr = buf->data() + ta->byteOffset + static_cast<size_t>(target.index) * width;
    SharedBlockRetainer retain{rtFindSharedStore(buf->externalPtrBits)};

    const auto deadline =
        target.hasDeadline ? Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                                std::chrono::duration<double, std::milli>(target.timeoutMs))
                           : Clock::time_point::max();

    auto node = std::make_shared<WaiterNode>();
    node->kind = WaiterKind::Sync;
    node->addr = addr;

    WaitTable& wt = waitTable();
    std::unique_lock<std::mutex> lock(wt.mu);
    if (loadSigned(addr, target.isBigInt) != target.expected) {
        lock.unlock();
        return rtMakeString("not-equal");
    }
    if (target.hasDeadline && target.timeoutMs <= 0.0) {
        lock.unlock();
        return rtMakeString("timed-out");
    }

    wt.queues[addr].push_back(node);
    struct QueueGuard {
        WaitTable& wt;
        const std::shared_ptr<WaiterNode>& node;
        std::unique_lock<std::mutex>& lock;
        ~QueueGuard() {
            if (!lock.owns_lock()) lock.lock();
            removeWaiterLocked(wt, node);
        }
    } guard{wt, node, lock};

    bool terminated = false;
    while (!node->notified) {
        if (rtTerminationRequested()) {
            terminated = true;
            break;
        }
        const auto slice = Clock::now() + std::chrono::milliseconds(50);
        const auto wakeAt = target.hasDeadline && deadline < slice ? deadline : slice;
        node->cv.wait_until(lock, wakeAt);
        if (node->notified) break;
        if (target.hasDeadline && Clock::now() >= deadline) break;
        lock.unlock();
        rtServiceHostInterrupt();
        lock.lock();
        if (node->notified) break;
    }
    const bool woke = node->notified;
    removeWaiterLocked(wt, node);
    lock.unlock();

    if (terminated) rtServiceInterrupt();
    return rtMakeString(woke ? "ok" : "timed-out");
}

Value rtAtomicsNotify(Value viewVal, Value indexVal, Value countVal) {
    Rooted<Value> view{viewVal};
    WaitTarget target;
    if (!validateWaitable("notify", view, indexVal, Value::fromUndefined(), Value::fromUndefined(),
                          /*requireShared=*/false, /*withValueAndTimeout=*/false, target)) {
        return Value::fromUndefined();
    }
    uint32_t count = std::numeric_limits<uint32_t>::max();
    if (!countVal.isUndefined()) {
        const double c = typed_array_internal::toInteger(rtToNumber(countVal));
        if (c <= 0.0) count = 0;
        else if (!std::isfinite(c) || c >= static_cast<double>(std::numeric_limits<uint32_t>::max())) {
            count = std::numeric_limits<uint32_t>::max();
        } else {
            count = static_cast<uint32_t>(c);
        }
    }
    auto* ta = view.get().asObject<TypedArrayHeader>();
    if (target.index >= ta->length) return Value::fromDouble(0.0);
    auto* buf = ta->buffer.asObject<ArrayBufferHeader>();
    if (!buf->isShared() || count == 0) return Value::fromDouble(0.0);

    const uint32_t width = target.isBigInt ? 8u : 4u;
    const void* addr = buf->data() + ta->byteOffset + static_cast<size_t>(target.index) * width;

    WaitTable& wt = waitTable();
    std::vector<std::shared_ptr<WaiterNode>> toNotify;
    {
        std::lock_guard<std::mutex> lock(wt.mu);
        auto it = wt.queues.find(addr);
        if (it == wt.queues.end()) return Value::fromDouble(0.0);
        auto& q = it->second;
        while (!q.empty() && toNotify.size() < count) {
            std::shared_ptr<WaiterNode> node = q.front();
            q.pop_front();
            node->removed = true;
            node->notified = true;
            toNotify.push_back(std::move(node));
        }
        if (q.empty()) wt.queues.erase(it);
    }
    for (const auto& node : toNotify) {
        if (node->kind == WaiterKind::Sync) node->cv.notify_one();
    }
    return Value::fromDouble(static_cast<double>(toNotify.size()));
}

Value rtAtomicsWaitAsync(Value viewVal, Value indexVal, Value expectedVal, Value timeoutVal) {
    Rooted<Value> view{viewVal};
    WaitTarget target;
    if (!validateWaitable("waitAsync", view, indexVal, expectedVal, timeoutVal, /*requireShared=*/true,
                          /*withValueAndTimeout=*/true, target)) {
        return Value::fromUndefined();
    }
    auto* ta = view.get().asObject<TypedArrayHeader>();
    if (target.index >= ta->length) return rtThrowRangeError("Invalid atomic access index");
    auto* buf = ta->buffer.asObject<ArrayBufferHeader>();
    const uint32_t width = target.isBigInt ? 8u : 4u;
    const void* addr = buf->data() + ta->byteOffset + static_cast<size_t>(target.index) * width;

    ensureAsyncWaiterRoots();
    Rooted<Value> promise{rtNewPromise()};
    auto node = std::make_shared<WaiterNode>();
    node->kind = WaiterKind::Async;
    node->addr = addr;
    node->block = rtFindSharedStore(buf->externalPtrBits);
    node->hasDeadline = target.hasDeadline;
    node->deadline =
        target.hasDeadline ? Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                                std::chrono::duration<double, std::milli>(target.timeoutMs))
                           : Clock::time_point::max();

    bool notEqual = false;
    bool zeroTimeout = false;
    {
        WaitTable& wt = waitTable();
        std::lock_guard<std::mutex> lock(wt.mu);
        if (loadSigned(addr, target.isBigInt) != target.expected) {
            notEqual = true;
        } else if (target.hasDeadline && target.timeoutMs <= 0.0) {
            zeroTimeout = true;
        } else {
            wt.queues[addr].push_back(node);
        }
    }
    if (notEqual) return makeWaitAsyncResult(false, rtMakeString("not-equal"));
    if (zeroTimeout) return makeWaitAsyncResult(false, rtMakeString("timed-out"));

    t_asyncWaiters.push_back(AsyncEntry{promise.get(), std::move(node)});
    return makeWaitAsyncResult(true, promise.get());
}

void rtDrainAsyncAtomicsWaiters() {
    if (t_asyncWaiters.empty()) return;
    const Clock::time_point now = Clock::now();
    WaitTable& wt = waitTable();

    std::vector<AsyncEntry> kept;
    kept.reserve(t_asyncWaiters.size());

    {
        std::lock_guard<std::mutex> lock(wt.mu);
        for (AsyncEntry& entry : t_asyncWaiters) {
            const auto& node = entry.node;
            if (node->notified) {
                t_settlingWaiters.emplace_back(entry.promise, true);
            } else if (node->hasDeadline && now >= node->deadline) {
                removeWaiterLocked(wt, node);
                t_settlingWaiters.emplace_back(entry.promise, false);
            } else {
                kept.push_back(std::move(entry));
            }
        }
    }
    t_asyncWaiters = std::move(kept);

    std::reverse(t_settlingWaiters.begin(), t_settlingWaiters.end());
    while (!t_settlingWaiters.empty()) {
        const auto [pVal, ok] = t_settlingWaiters.back();
        t_settlingWaiters.pop_back();
        Rooted<Value> p{pVal};
        Rooted<Value> res{rtMakeString(ok ? "ok" : "timed-out")};
        rtResolvePromise(p, res);
    }
}

void rtClearThreadAsyncAtomicsWaiters() noexcept {
    t_settlingWaiters.clear();
    if (t_asyncWaiters.empty()) return;
    WaitTable& wt = waitTable();
    std::lock_guard<std::mutex> lock(wt.mu);
    for (const AsyncEntry& entry : t_asyncWaiters) {
        removeWaiterLocked(wt, entry.node);
    }
    t_asyncWaiters.clear();
}

}  // namespace bronze::runtime
