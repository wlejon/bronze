#pragma once

// GC-safe object and array builders on the embed surface. Both root their
// receiver in a Persistent and re-store the post-allocation address on every
// mutating call, so callers can populate properties and elements in a fixed
// deterministic order without hand-managing roots across moving collections.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "embed/embed.h"

namespace bronze::embed {

struct ObjectBuilder {
    Persistent obj;

    ObjectBuilder() : obj(createObject()) {}
    explicit ObjectBuilder(Value existing) : obj(existing) {}

    void set(std::string_view name, Value v) {
        Persistent valP(v);
        obj.set(setProperty(obj.get(), name, valP.get()));
    }

    void set(std::string_view name, double d) {
        set(name, fromDouble(d));
    }

    void set(std::string_view name, int32_t i) {
        set(name, fromDouble(static_cast<double>(i)));
    }

    void set(std::string_view name, uint32_t u) {
        set(name, fromDouble(static_cast<double>(u)));
    }

    void set(std::string_view name, uint16_t u) {
        set(name, fromDouble(static_cast<double>(u)));
    }

    void set(std::string_view name, int64_t i) {
        set(name, fromDouble(static_cast<double>(i)));
    }

    void set(std::string_view name, uint64_t u) {
        set(name, fromDouble(static_cast<double>(u)));
    }

    void set(std::string_view name, bool b) {
        set(name, fromBool(b));
    }

    void set(std::string_view name, const std::string& s) {
        set(name, fromUtf8(s));
    }

    void set(std::string_view name, const char* s) {
        set(name, fromUtf8(s ? s : ""));
    }

    void set(std::string_view name, std::string_view s) {
        set(name, fromUtf8(s));
    }

    void setNull(std::string_view name) {
        set(name, null());
    }

    void setUndefined(std::string_view name) {
        set(name, undefined());
    }

    void def(std::string_view name, uint32_t arity, NativeFn fn) {
        Persistent f(makeFunction(std::move(fn), arity, name));
        obj.set(setProperty(obj.get(), name, f.get()));
    }

    void accessor(std::string_view name, NativeFn getter, NativeFn setter = nullptr) {
        const std::string getName = "get " + std::string(name);
        const std::string setName = "set " + std::string(name);
        Persistent g(makeFunction(std::move(getter), 0, getName));
        Persistent s(setter ? makeFunction(std::move(setter), 1, setName) : undefined());
        obj.set(defineAccessor(obj.get(), name, g.get(), s.get(), /*enumerable=*/true));
    }

    Value get() const { return obj.get(); }
    Value build() const { return obj.get(); }
};

struct ArrayBuilder {
    Persistent arr;
    uint32_t length = 0;

    ArrayBuilder() : arr(makeArray(0)), length(0) {}
    explicit ArrayBuilder(size_t count)
        : arr(makeArray(static_cast<uint32_t>(count))),
          length(static_cast<uint32_t>(count)) {}

    void set(uint32_t index, Value v) {
        Persistent valP(v);
        arr.set(setElement(arr.get(), index, valP.get()));
        if (index >= length) length = index + 1;
    }

    void set(uint32_t index, double d) {
        set(index, fromDouble(d));
    }

    void set(uint32_t index, int32_t i) {
        set(index, fromDouble(static_cast<double>(i)));
    }

    void set(uint32_t index, uint32_t u) {
        set(index, fromDouble(static_cast<double>(u)));
    }

    void set(uint32_t index, int64_t i) {
        set(index, fromDouble(static_cast<double>(i)));
    }

    void set(uint32_t index, uint64_t u) {
        set(index, fromDouble(static_cast<double>(u)));
    }

    void set(uint32_t index, bool b) {
        set(index, fromBool(b));
    }

    void set(uint32_t index, const std::string& s) {
        set(index, fromUtf8(s));
    }

    void set(uint32_t index, const char* s) {
        set(index, fromUtf8(s ? s : ""));
    }

    void set(uint32_t index, std::string_view s) {
        set(index, fromUtf8(s));
    }

    void push(Value v) {
        set(length, v);
    }

    void push(double d) {
        push(fromDouble(d));
    }

    void push(int32_t i) {
        push(fromDouble(static_cast<double>(i)));
    }

    void push(uint32_t u) {
        push(fromDouble(static_cast<double>(u)));
    }

    void push(int64_t i) {
        push(fromDouble(static_cast<double>(i)));
    }

    void push(uint64_t u) {
        push(fromDouble(static_cast<double>(u)));
    }

    void push(bool b) {
        push(fromBool(b));
    }

    void push(const std::string& s) {
        push(fromUtf8(s));
    }

    void push(const char* s) {
        push(fromUtf8(s ? s : ""));
    }

    void push(std::string_view s) {
        push(fromUtf8(s));
    }

    Value get() const { return arr.get(); }
    Value build() const { return arr.get(); }
};

}  // namespace bronze::embed
