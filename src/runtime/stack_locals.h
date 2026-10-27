#pragma once

// Per-STACK runtime state (brass/gc/fiber.hpp, "stack-local state").
//
// Some of the runtime's thread_locals are pushed and popped in step with the
// frames of one stack: the shadow stack's innermost frame, the realm stack,
// the embed handle scopes, a native call's scratch. A compartment runs its
// entries on a fiber of its own (runtime/compartment.h), so a thread has
// as many of those stacks as it has compartments; each such variable is
// registered with brass, which saves it for the stack a switch leaves and
// restores the one it enters. These helpers register the two shapes the
// runtime has: a pointer (or other word) and a container a stack owns.
//
// A registration is a static of the defining translation unit, made before
// main and so before any fiber exists.

#include <brass/gc/fiber.hpp>

#include <cstring>
#include <utility>

namespace bronze::runtime {

// A word-sized variable: `Get()` names this thread's copy. A fresh stack
// starts with `Fresh()`.
template <typename T, T& (*Get)(), T (*Fresh)()>
int registerStackLocalWord() {
    static_assert(sizeof(T) <= sizeof(void*), "a stack-local word must fit a pointer");
    brass::gc::StackLocal local;
    local.save = [](void** slot) {
        void* bits = nullptr;
        T value = Get();
        std::memcpy(&bits, &value, sizeof(T));
        *slot = bits;
    };
    local.restore = [](void** slot, bool fresh) {
        if (fresh) {
            Get() = Fresh();
            return;
        }
        T value;
        void* bits = *slot;
        std::memcpy(&value, &bits, sizeof(T));
        Get() = value;
    };
    return brass::gc::register_stack_local(local);
}

// A container (or any movable value with a cheap empty state): the stack
// that leaves keeps its contents in a box of its own until it is entered
// again; a fresh stack starts with an empty one. `Get()` names this
// thread's copy.
template <typename T, T& (*Get)()>
int registerStackLocalBox() {
    brass::gc::StackLocal local;
    local.save = [](void** slot) {
        T* box = static_cast<T*>(*slot);
        if (box == nullptr) {
            box = new T();
            *slot = box;
        }
        using std::swap;
        swap(*box, Get());
        // What the box held (left there by this stack's last restore) is
        // the empty state, now this thread's until the next restore.
    };
    local.restore = [](void** slot, bool fresh) {
        T* box = static_cast<T*>(*slot);
        if (fresh || box == nullptr) {
            Get() = T();
            return;
        }
        using std::swap;
        swap(*box, Get());
    };
    local.release = [](void* value) { delete static_cast<T*>(value); };
    return brass::gc::register_stack_local(local);
}

// What a switched-out stack holds for a registered box (or word), for a
// root walk: fn(value) for each stack of the thread other than the running
// one. A box's value is a T*; a word's is its bits.
template <typename F>
void forEachSwitchedStackLocal(int index, F&& fn) {
    brass::gc::for_each_switched_stack_local(
        index, [](void* ctx, void* value) { (*static_cast<F*>(ctx))(value); }, &fn);
}

}  // namespace bronze::runtime
