#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <type_traits>

namespace bronze {

enum class Tag : uint16_t {
    Object    = 0xFFF1,
    String    = 0xFFF2,
    Int32     = 0xFFF3,
    Bool      = 0xFFF4,
    Null      = 0xFFF5,
    Undefined = 0xFFF6,
    Hole      = 0xFFF7,
    Symbol    = 0xFFF8,
};

constexpr uint64_t kCanonicalNaNBits = 0x7FF8000000000000ULL;
constexpr uint64_t kNumberMaxBits    = 0xFFF0000000000000ULL;
constexpr uint64_t kPayloadMask      = 0x0000FFFFFFFFFFFFULL;
constexpr uint32_t kTagShift         = 48;

class Value {
public:
    constexpr Value() noexcept : bits_(static_cast<uint64_t>(Tag::Undefined) << kTagShift) {}
    constexpr explicit Value(uint64_t bits) noexcept : bits_(bits) {}

    static Value fromDouble(double d) noexcept {
        if (std::isnan(d)) {
            return Value(kCanonicalNaNBits);
        }
        return Value(std::bit_cast<uint64_t>(d));
    }

    static Value fromBool(bool b) noexcept {
        return Value((static_cast<uint64_t>(Tag::Bool) << kTagShift) | (b ? 1ULL : 0ULL));
    }

    static constexpr Value fromNull() noexcept {
        return Value(static_cast<uint64_t>(Tag::Null) << kTagShift);
    }

    static constexpr Value fromUndefined() noexcept {
        return Value(static_cast<uint64_t>(Tag::Undefined) << kTagShift);
    }

    static constexpr Value fromHole() noexcept {
        return Value(static_cast<uint64_t>(Tag::Hole) << kTagShift);
    }

    static Value fromObject(const void* ptr) noexcept {
        auto addr = reinterpret_cast<uintptr_t>(ptr);
        return Value((static_cast<uint64_t>(Tag::Object) << kTagShift) | (addr & kPayloadMask));
    }

    static Value fromString(const void* ptr) noexcept {
        auto addr = reinterpret_cast<uintptr_t>(ptr);
        return Value((static_cast<uint64_t>(Tag::String) << kTagShift) | (addr & kPayloadMask));
    }

    static Value fromSymbol(const void* ptr) noexcept {
        auto addr = reinterpret_cast<uintptr_t>(ptr);
        return Value((static_cast<uint64_t>(Tag::Symbol) << kTagShift) | (addr & kPayloadMask));
    }

    static constexpr Value fromTagAndPayload(uint16_t tag, uint64_t payload) noexcept {
        return Value((static_cast<uint64_t>(tag) << kTagShift) | (payload & kPayloadMask));
    }

    static constexpr Value fromRawBits(uint64_t bits) noexcept {
        return Value(bits);
    }

    constexpr uint64_t rawBits() const noexcept { return bits_; }

    constexpr uint16_t tag() const noexcept {
        return static_cast<uint16_t>(bits_ >> kTagShift);
    }

    constexpr uint64_t payload() const noexcept {
        return bits_ & kPayloadMask;
    }

    constexpr bool isNumber() const noexcept {
        return bits_ <= kNumberMaxBits;
    }

    double asNumber() const noexcept {
        return std::bit_cast<double>(bits_);
    }

    constexpr bool isBool() const noexcept {
        return tag() == static_cast<uint16_t>(Tag::Bool);
    }

    constexpr bool asBool() const noexcept {
        return (bits_ & 1ULL) != 0;
    }

    constexpr bool isNull() const noexcept {
        return tag() == static_cast<uint16_t>(Tag::Null);
    }

    constexpr bool isUndefined() const noexcept {
        return tag() == static_cast<uint16_t>(Tag::Undefined);
    }

    constexpr bool isHole() const noexcept {
        return tag() == static_cast<uint16_t>(Tag::Hole);
    }

    constexpr bool isObject() const noexcept {
        return tag() == static_cast<uint16_t>(Tag::Object);
    }

    template <typename T = void>
    T* asObject() const noexcept {
        return reinterpret_cast<T*>(static_cast<uintptr_t>(payload()));
    }

    constexpr bool isString() const noexcept {
        return tag() == static_cast<uint16_t>(Tag::String);
    }

    template <typename T = void>
    T* asString() const noexcept {
        return reinterpret_cast<T*>(static_cast<uintptr_t>(payload()));
    }

    constexpr bool isSymbol() const noexcept {
        return tag() == static_cast<uint16_t>(Tag::Symbol);
    }

    template <typename T = void>
    T* asSymbol() const noexcept {
        return reinterpret_cast<T*>(static_cast<uintptr_t>(payload()));
    }

    constexpr bool isInt32() const noexcept {
        return tag() == static_cast<uint16_t>(Tag::Int32);
    }

    constexpr bool isPointer() const noexcept {
        return isObject() || isString() || isSymbol();
    }

    constexpr bool operator==(const Value& other) const noexcept = default;

private:
    uint64_t bits_;
};

static_assert(sizeof(Value) == 8, "Value must be 64-bit");

}  // namespace bronze
