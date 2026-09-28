#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "types/type.h"

namespace bronze::types {

// `name -> Type` at one program point: the flow analysis's environment.
//
// A sorted vector rather than a std::map because the analysis COPIES it far
// more often than it edits it — every branch saves the entry environment,
// every merge builds a new one, every loop round snapshots its header — and a
// map copy allocates a node (and usually a string) per binding. Here a copy
// is one allocation; the names are short enough that their strings live
// inline. Iteration is in name order, the order a std::map<std::string, Type>
// gives, so every dump and every join sees the bindings in the same sequence
// it always did.
class Env {
public:
    using value_type = std::pair<std::string, Type>;
    using Storage = std::vector<value_type>;
    using iterator = Storage::iterator;
    using const_iterator = Storage::const_iterator;

    iterator begin() { return entries_.begin(); }
    iterator end() { return entries_.end(); }
    const_iterator begin() const { return entries_.begin(); }
    const_iterator end() const { return entries_.end(); }
    size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }
    void clear() { entries_.clear(); }

    iterator find(std::string_view name) {
        const auto it = lowerBound(name);
        return it != entries_.end() && it->first == name ? it : entries_.end();
    }
    const_iterator find(std::string_view name) const {
        const auto it = lowerBound(name);
        return it != entries_.end() && it->first == name ? it : entries_.end();
    }
    size_t count(std::string_view name) const { return find(name) != entries_.end() ? 1 : 0; }

    // As std::map: a name not yet bound is inserted holding `Type()` (Never).
    Type& operator[](std::string_view name) {
        auto it = lowerBound(name);
        if (it == entries_.end() || it->first != name) {
            it = entries_.emplace(it, std::string(name), Type());
        }
        return it->second;
    }
    // As std::map: an existing binding is left alone.
    std::pair<iterator, bool> emplace(std::string_view name, Type t) {
        auto it = lowerBound(name);
        if (it != entries_.end() && it->first == name) return {it, false};
        return {entries_.emplace(it, std::string(name), t), true};
    }
    size_t erase(std::string_view name) {
        const auto it = find(name);
        if (it == entries_.end()) return 0;
        entries_.erase(it);
        return 1;
    }

    friend bool operator==(const Env& a, const Env& b) { return a.entries_ == b.entries_; }
    friend bool operator!=(const Env& a, const Env& b) { return !(a == b); }

    // Appends a binding that sorts after every one already here: the join
    // builds its result in order, so it needs no search.
    void appendSorted(std::string name, Type t) { entries_.emplace_back(std::move(name), t); }
    void reserve(size_t n) { entries_.reserve(n); }

private:
    iterator lowerBound(std::string_view name) {
        return std::lower_bound(entries_.begin(), entries_.end(), name,
                                [](const value_type& e, std::string_view n) { return std::string_view(e.first) < n; });
    }
    const_iterator lowerBound(std::string_view name) const {
        return std::lower_bound(entries_.begin(), entries_.end(), name,
                                [](const value_type& e, std::string_view n) { return std::string_view(e.first) < n; });
    }

    Storage entries_;
};

}  // namespace bronze::types
