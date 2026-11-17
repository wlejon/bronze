#pragma once

// The builtins with a FAST ENTRY (runtime/native_fast.h): a second door into
// a native builtin for a compiled method site, the receiver and arguments in
// registers. Lowering splits each site of one on its receiver kind and calls
// the entry on the passing edge (lower/selfhost_merge.cpp); the whole-program
// optimizer reads a call of an entry as the method call it replaced, the
// builtin at `builtin` called with the first operand as `this`
// (opt/wpo/solver_calls.cpp).

#include <cstdint>
#include <string_view>

namespace bronze::il {

// The receiver a site's guard establishes: an Array at the bare array shape
// (`is.plain_array`), a string or a number primitive (`typeof.is`). On each
// the method's [[Get]] reaches a frozen intrinsic prototype (runtime/lockdown.h)
// with nothing in between, so it answers the builtin.
// A RegExp at the intrinsic instance shape (`is.collection` of
// BRONZE_ABI_COLLECTION_REGEXP): no own key, so its members are the frozen
// RegExp.prototype's.
enum class FastReceiver { PlainArray, String, Number, RegExp };

struct FastEntry {
    const char* method;
    const char* symbol;
    FastReceiver receiver;
    // The arguments the entry takes after the receiver: a site passing up to
    // this many is split, the rest padded with undefined (native_fast.h says
    // why that is the absent argument for each).
    uint32_t argc;
    // The builtin's path in the optimizer's catalogue (opt/wpo/builtins.cpp).
    const char* builtin;
};

// `push` and `pop` have none: the method site's inline path answers them
// (codegen-brass/il2mir/il_call_entry.cpp). A method named twice (`indexOf`)
// is tested for its kinds in this order.
inline constexpr FastEntry kFastEntries[] = {
    {"indexOf", "bronze_fast_array_index_of", FastReceiver::PlainArray, 1, "Array.prototype.indexOf"},
    {"includes", "bronze_fast_array_includes", FastReceiver::PlainArray, 1, "Array.prototype.includes"},
    {"join", "bronze_fast_array_join", FastReceiver::PlainArray, 1, "Array.prototype.join"},
    {"charCodeAt", "bronze_fast_string_char_code_at", FastReceiver::String, 1, "String.prototype.charCodeAt"},
    {"charAt", "bronze_fast_string_char_at", FastReceiver::String, 1, "String.prototype.charAt"},
    {"indexOf", "bronze_fast_string_index_of", FastReceiver::String, 2, "String.prototype.indexOf"},
    {"startsWith", "bronze_fast_string_starts_with", FastReceiver::String, 2, "String.prototype.startsWith"},
    {"slice", "bronze_fast_string_slice", FastReceiver::String, 2, "String.prototype.slice"},
    {"substring", "bronze_fast_string_substring", FastReceiver::String, 2, "String.prototype.substring"},
    {"split", "bronze_fast_string_split", FastReceiver::String, 2, "String.prototype.split"},
    {"trim", "bronze_fast_string_trim", FastReceiver::String, 0, "String.prototype.trim"},
    {"toString", "bronze_fast_number_to_string", FastReceiver::Number, 1, "Number.prototype.toString"},
    {"exec", "bronze_fast_regexp_exec", FastReceiver::RegExp, 1, "RegExp.prototype.exec"},
    {"test", "bronze_fast_regexp_test", FastReceiver::RegExp, 1, "RegExp.prototype.test"},
};

// The row of the entry named `symbol`, or null.
inline const FastEntry* fastEntryNamed(std::string_view symbol) {
    for (const FastEntry& e : kFastEntries) {
        if (symbol == e.symbol) return &e;
    }
    return nullptr;
}

}  // namespace bronze::il
