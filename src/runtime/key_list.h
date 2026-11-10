#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

#include "runtime/property_key.h"
#include "runtime/string.h"

namespace brass::gc {
class Tracer;
}

namespace bronze {

// A list of property keys that is a GC ROOT for as long as it lives.
//
// Why own-key lists need one. A transition-tree key is an arena string,
// immortal, so a list of them could be a plain vector and stay valid across
// anything. A DICTIONARY key is not: it is an ordinary string of the old
// generation (dictionary.h), alive because the entry names it, and dead at the
// next full collection once a `delete` has removed the entry. A snapshot of an
// object's keys outlives exactly such deletes whenever program code runs while
// it is walked — a getter `Object.entries` calls, a proxy trap, a `toJSON` —
// and a vector's storage is C++ heap memory, which the collector does not read
// (a stack word naming a key keeps it, gc_contract.md 2.5; a vector element
// does not). So every own-key snapshot is one of these: the thread keeps a list
// of the live ones and a root source visits each element (heap.cpp,
// registerFrameRoots), which marks a heap key and passes over an arena one.
//
// The element is a raw pointer to a key's header (a PropertyKey or a
// StringHeader*), and old keys never move, so the visit only marks. It is not
// convertible to a std::vector on purpose: copying a snapshot out of one is how
// a key would lose its root without anything saying so.
class KeyListLink {
public:
    KeyListLink(const KeyListLink&) = delete;
    KeyListLink& operator=(const KeyListLink&) = delete;

    // Visits every key of every live list of this thread (heap.cpp).
    static void visitAll(brass::gc::Tracer& t) noexcept;

protected:
    KeyListLink() noexcept;
    ~KeyListLink();
    // The list's elements, each one pointer word naming a key's header.
    virtual void words(uintptr_t*& data, size_t& count) noexcept = 0;

private:
    KeyListLink* prev_{nullptr};
    KeyListLink* next_{nullptr};
};

template <typename T>
class KeyList final : public KeyListLink {
    static_assert(sizeof(T) == sizeof(uintptr_t) && std::is_trivially_copyable_v<T>,
                  "a key list element is one pointer word naming a key");

public:
    using value_type = T;
    using iterator = typename std::vector<T>::iterator;
    using const_iterator = typename std::vector<T>::const_iterator;

    KeyList() noexcept = default;
    KeyList(const KeyList& other) : v_(other.v_) {}
    KeyList(KeyList&& other) noexcept : v_(std::move(other.v_)) {}
    KeyList& operator=(const KeyList& other) {
        v_ = other.v_;
        return *this;
    }
    KeyList& operator=(KeyList&& other) noexcept {
        v_ = std::move(other.v_);
        return *this;
    }
    ~KeyList() = default;

    iterator begin() noexcept { return v_.begin(); }
    iterator end() noexcept { return v_.end(); }
    const_iterator begin() const noexcept { return v_.begin(); }
    const_iterator end() const noexcept { return v_.end(); }
    size_t size() const noexcept { return v_.size(); }
    bool empty() const noexcept { return v_.empty(); }
    T& operator[](size_t i) noexcept { return v_[i]; }
    const T& operator[](size_t i) const noexcept { return v_[i]; }
    T& front() noexcept { return v_.front(); }
    T& back() noexcept { return v_.back(); }
    void push_back(T k) { v_.push_back(k); }
    void reserve(size_t n) { v_.reserve(n); }
    void clear() noexcept { v_.clear(); }
    void swap(KeyList& other) noexcept { v_.swap(other.v_); }

private:
    void words(uintptr_t*& data, size_t& count) noexcept override {
        data = reinterpret_cast<uintptr_t*>(v_.data());
        count = v_.size();
    }

    std::vector<T> v_;
};

using PropertyKeyList = KeyList<PropertyKey>;
using StringKeyList = KeyList<StringHeader*>;

}  // namespace bronze
