#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

#include "runtime/heap.h"
#include "runtime/value.h"

namespace bronze {

class ShadowStackFrame {
public:
    ShadowStackFrame() noexcept;
    ~ShadowStackFrame() noexcept;

    ShadowStackFrame(const ShadowStackFrame&) = delete;
    ShadowStackFrame& operator=(const ShadowStackFrame&) = delete;

    ShadowStackFrame* prev() const noexcept { return prev_; }
    Value** roots() const noexcept { return const_cast<Value**>(roots_.data()); }
    size_t count() const noexcept { return roots_.size(); }

    void push(Value* slot);
    void pop(Value* slot);

    static ShadowStackFrame* current() noexcept;

private:
    ShadowStackFrame* prev_{nullptr};
    std::vector<Value*> roots_;
    static thread_local ShadowStackFrame* top_frame_;
};

// The generated-code half of the shadow stack (docs/0006) is the
// bronze_gc_frame linked list declared in the ABI registry: compiled
// functions allocate a frame in their own stack frame and link it onto
// bronze_gc_frame_top inline, with no helper call. The collector walks
// that list alongside the ShadowStackFrame chain above. Contiguous slots
// rather than a vector of slot pointers because generated code cannot
// build a vector — and does not need to: its slot count is a compile-time
// constant.

template <typename T = Value>
class Rooted {
public:
    Rooted() : slot_() {
        register_slot();
    }

    explicit Rooted(T value) : slot_(to_value(value)) {
        register_slot();
    }

    Rooted(Heap& heap, T value) : slot_(to_value(value)) {
        (void)heap;
        register_slot();
    }

    explicit Rooted(Heap& heap) : slot_() {
        (void)heap;
        register_slot();
    }

    ~Rooted() {
        unregister_slot();
    }

    Rooted(const Rooted&) = delete;
    Rooted& operator=(const Rooted&) = delete;

    Rooted(Rooted&& other) noexcept : slot_(other.slot_) {
        register_slot();
    }

    Rooted& operator=(T value) {
        slot_ = to_value(value);
        return *this;
    }

    T get() const noexcept {
        return from_value(slot_);
    }

    void set(T value) noexcept {
        slot_ = to_value(value);
    }

    Value& slot() noexcept { return slot_; }
    const Value& slot() const noexcept { return slot_; }
    Value* slot_ptr() noexcept { return &slot_; }

    T operator*() const noexcept { return get(); }

    auto operator->() const noexcept {
        if constexpr (std::is_pointer_v<T>) {
            return get();
        } else {
            return &slot_;
        }
    }

private:
    static Value to_value(T val) {
        if constexpr (std::is_same_v<T, Value>) {
            return val;
        } else if constexpr (std::is_pointer_v<T>) {
            return Value::fromObject(reinterpret_cast<const void*>(val));
        } else {
            return Value::fromRawBits(static_cast<uint64_t>(val));
        }
    }

    static T from_value(Value val) {
        if constexpr (std::is_same_v<T, Value>) {
            return val;
        } else if constexpr (std::is_pointer_v<T>) {
            return val.asObject<std::remove_pointer_t<T>>();
        } else {
            return static_cast<T>(val.rawBits());
        }
    }

    void register_slot() {
        frame_ = ShadowStackFrame::current();
        if (frame_) {
            frame_->push(&slot_);
        }
    }

    void unregister_slot() {
        if (frame_) {
            frame_->pop(&slot_);
            frame_ = nullptr;
        }
    }

    Value slot_;
    ShadowStackFrame* frame_{nullptr};
};

}  // namespace bronze
