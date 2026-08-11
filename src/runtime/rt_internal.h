#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

// The runtime's process-wide state — the heap, the non-moving arena, the
// root shapes, the key registry and the collector's root sources — is owned
// by ONE translation unit (rt_state.cpp), so its construction order is that
// unit's business alone. Every other runtime translation unit reaches it
// through the accessors below rather than declaring statics of its own,
// which would put the collector's roots at the mercy of cross-TU
// initialization order.
//
// Nothing here is part of the generated-code ABI: that is bronze_abi.h, and
// it stays pure C. This header is C++ and internal to src/runtime.

namespace bronze::runtime {

Heap& rtHeap();
NonMovingArena& rtArena();

// A root shape registered with the collector, for a builtin that needs its
// own hidden class rather than the one every `{}` literal shares.
Shape* rtNewRootShape(Value proto);

// The one root shape every plain `{}` literal starts from. Per-literal root
// shapes would give two identical literals unrelated hidden classes, so a
// site seeing both would miss its inline cache every time (docs/0008
// decision 1).
Shape* rtPlainObjectShape();

// A property key by the index lowering assigned it. The string form is for
// comparisons; the header form is the arena-interned key the property path
// uses, so a property access allocates nothing. `rtKeyHeader` is null for an
// index no `bronze_register_key_string` call ever covered.
const std::string& rtKeyString(uint32_t index);
StringHeader* rtKeyHeader(uint32_t index);

// Is this key an ARRAY INDEX spelled as a string? Enumeration order asks it
// (docs/0009) and so does console.log of an object, which reports the same
// order — one test, so the two answers cannot drift.
bool rtIsIntegerLikeKey(std::string_view key, uint32_t& out);

// console.log of a container, in the format docs/0013 pins. Returns the
// text; the caller writes it.
std::string rtInspect(Value v);

// A heap string from UTF-8 bytes, and JS ToString / ToNumber. ToString on
// an object and ToNumber on an object are hard errors: both need
// ToPrimitive, which bronze has not built.
Value rtMakeString(std::string_view utf8);
Value rtValueToString(Value v);
double rtToNumber(Value v);

// ECMA-262 Number::exponentiate, which `**` and `Math.pow` are both defined
// as. One implementation, because the two must not drift: C's pow disagrees
// with it on a NaN exponent and on a base of magnitude 1 with an infinite
// one (rt_operator.cpp).
double rtExponentiate(double base, double exponent);

// The characters of a string as bytes, with any code unit past U+007F
// replaced by 0xFF — enough for the numeric and structural parsing the
// builtins do, and never enough to be mistaken for a general conversion.
std::string rtAsciiChars(const StringHeader* s);

// Diagnose `key` if it is a real member of `receiver` that bronze has not
// implemented; return quietly otherwise, so the caller reads `undefined`,
// which is what the language says for a property that does not exist.
// The tables are the ECMA-262 question "does this member exist?", never
// "have we got round to it?" — see rt_members.cpp.
void rtCheckUnimplementedMember(const char* receiver, const char* const* names, size_t count,
                                const std::string& key);

// The same check against each prototype rt_members.cpp carries a table for.
// A member that lands leaves its table, so these only ever fire for names
// ECMA-262 defines and bronze has not built.
void rtCheckArrayMember(const std::string& key);
void rtCheckStringMember(const std::string& key);
void rtCheckTypedArrayMember(const std::string& key);
void rtCheckArrayBufferMember(const std::string& key);
void rtCheckFunctionMember(const std::string& key);

// A function's `.prototype` and its own-property object, created on first
// demand: a function that is never used as a constructor and never given a
// static member pays for neither (docs/0008 decision 4, docs/0012 decision
// 6). Both allocate, so both take the function through a root and the
// caller must re-derive any raw pointer afterwards.
void rtEnsureFunctionPrototype(Rooted<Value>& fnVal);
void rtEnsureFunctionProperties(Rooted<Value>& fnVal);

// A native builtin's prologue. bronze_dynamic_call hands a builtin an
// argument block that is rooted only as long as the CALLER's frame is —
// generated code's block lives in its GC root frame (docs/0006), but
// FunctionHeader::call's arity-adaptation vector and the blocks builtins
// build for callbacks are plain stack memory. The contract that makes both
// safe is that the callee copies its parameters into roots of its own
// before it allocates, and this is that copy, made explicit: after
// constructing one, read arguments from HERE and never from `argv` again.
//
// The std::vector allocation is C++'s, not the bronze heap's, so no
// collection can happen between the copy and the rooting.
class RootedArgs {
public:
    RootedArgs(uint32_t argc, const uint64_t* argv) : slots_(argc) {
        for (uint32_t i = 0; i < argc; ++i) slots_[i] = Value(argv[i]);
        frame_ = ShadowStackFrame::current();
        if (frame_) {
            for (Value& slot : slots_) frame_->push(&slot);
        }
    }

    ~RootedArgs() {
        if (frame_) {
            for (Value& slot : slots_) frame_->pop(&slot);
        }
    }

    RootedArgs(const RootedArgs&) = delete;
    RootedArgs& operator=(const RootedArgs&) = delete;

    uint32_t count() const noexcept { return static_cast<uint32_t>(slots_.size()); }

    // Out of range is `undefined`, which is what a JS call site that omitted
    // the argument means — so a builtin never has to bounds-check first.
    Value operator[](uint32_t i) const noexcept {
        return i < slots_.size() ? slots_[i] : Value::fromUndefined();
    }
    Value at(uint32_t i, Value fallback) const noexcept {
        return i < slots_.size() ? slots_[i] : fallback;
    }

private:
    std::vector<Value> slots_;
    ShadowStackFrame* frame_{nullptr};
};

// ---- builtin namespaces ---------------------------------------------------
// Each family owns its own translation unit and exposes exactly two things:
// the namespace object, and the miss check that keeps an unimplemented
// member loud instead of `undefined`.

Value rtMathObject();
void rtMathCheckMissingMember(Value obj, const std::string& key);

// `undefined` for a name that is not an implemented method, so the property
// path can fall through to the unimplemented-member table and then to the
// language's own answer for a property that does not exist.
Value rtArrayMethod(const std::string& key);
Value rtStringMethod(const std::string& key);

}  // namespace bronze::runtime
