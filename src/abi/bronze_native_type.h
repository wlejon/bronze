#pragma once

// The native-call type vocabulary: the ONE spelling of every type a host may
// give a native's parameter or return, shared by the three places that read
// it — the runtime registry that validates a registration, the lowerer that
// maps a manifest onto IL, and the manifest writer that prints the registry
// back out. Header-only and beside the ABI header on purpose: the runtime
// links nothing above `abi`, and the lowerer links nothing below `il`, so a
// vocabulary either of them had to link a library for would sit in neither.
//
// Closed and checked. A spelling not listed here is a REGISTRATION ERROR (or
// a manifest error), never `dynamic`: "float", "number", "int" and the rest
// of the aliases the prototype accepted are gone, because a type string that
// silently means "whatever" is how a native ends up reading a boxed Value as
// a double.
//
// How each kind crosses the call (the C signature a native must have):
//
//   void      return only                    void
//   f64       double                         double
//   i32       ToInt32 of the argument        int32_t
//   bool      ToBoolean of the argument      bool (passed as int 0/1; a
//                                            returned bool is masked to its
//                                            low bit, so `al` alone is read)
//   str       UTF-8, NUL-terminated          const char* — valid for the
//                                            call only (a thread ring of 16)
//   dynamic   the raw Value bits             uint64_t — the callee must not
//                                            retain them across any call
//                                            that can allocate
//   T[]       a typed array of exactly T     (T* data, uint32_t length) — TWO
//                                            C parameters; a wrong element
//                                            kind or a detached buffer is a
//                                            TypeError before the call; valid
//                                            for the call only, and the
//                                            native must not allocate through
//                                            the embed API while holding it
//   <class>   a handle made by that class's  void* (the data the constructor
//             constructor                    returned); a value of another
//                                            class, a plain object, or a
//                                            missing argument is a TypeError
//                                            naming the class. As a RETURN
//                                            type: the native returns void*
//                                            and the runtime wraps it in a
//                                            handle of that class (null →
//                                            null).
//
// Typed arrays are parameter-only: a native has no buffer to hand back.

#include <cstdint>
#include <string>
#include <string_view>

namespace bronze::abi {

enum class NativeType : uint8_t {
    Void,
    F64,
    I32,
    Bool,
    Str,
    Dynamic,
    F32Array,
    F64Array,
    I32Array,
    U8Array,
    U16Array,
    U32Array,
    I8Array,
    I16Array,
    // A class-tagged handle. `className` on the ref names the class.
    Class,
};

struct NativeTypeRef {
    NativeType kind = NativeType::Void;
    std::string className;  // Class only: the registered class path
};

inline bool nativeTypeIsTypedArray(NativeType t) {
    switch (t) {
        case NativeType::F32Array:
        case NativeType::F64Array:
        case NativeType::I32Array:
        case NativeType::U8Array:
        case NativeType::U16Array:
        case NativeType::U32Array:
        case NativeType::I8Array:
        case NativeType::I16Array:
            return true;
        default:
            return false;
    }
}

// The runtime's ElementKind numbering (src/runtime/typed_array.h), which is
// the ABI's numbering too: Int8=0, Uint8=1, Uint8Clamped=2, Int16=3,
// Uint16=4, Int32=5, Uint32=6, Float32=7, Float64=8. Only the eight the
// vocabulary names are reachable here.
inline uint32_t nativeTypedArrayElementKind(NativeType t) {
    switch (t) {
        case NativeType::I8Array: return 0;
        case NativeType::U8Array: return 1;
        case NativeType::I16Array: return 3;
        case NativeType::U16Array: return 4;
        case NativeType::I32Array: return 5;
        case NativeType::U32Array: return 6;
        case NativeType::F32Array: return 7;
        case NativeType::F64Array: return 8;
        default: return 0xFFFFFFFFu;
    }
}

// The fixed vocabulary, or nothing. A spelling that is not one of these is
// EITHER a class name (the caller decides, against the classes it knows) OR
// an error; this function does not guess, which is the whole point of it.
inline bool parseNativeScalarType(std::string_view text, NativeType& out) {
    if (text == "void") { out = NativeType::Void; return true; }
    if (text == "f64") { out = NativeType::F64; return true; }
    if (text == "i32") { out = NativeType::I32; return true; }
    if (text == "bool") { out = NativeType::Bool; return true; }
    if (text == "str") { out = NativeType::Str; return true; }
    if (text == "dynamic") { out = NativeType::Dynamic; return true; }
    if (text == "f32[]") { out = NativeType::F32Array; return true; }
    if (text == "f64[]") { out = NativeType::F64Array; return true; }
    if (text == "i32[]") { out = NativeType::I32Array; return true; }
    if (text == "u8[]") { out = NativeType::U8Array; return true; }
    if (text == "u16[]") { out = NativeType::U16Array; return true; }
    if (text == "u32[]") { out = NativeType::U32Array; return true; }
    if (text == "i8[]") { out = NativeType::I8Array; return true; }
    if (text == "i16[]") { out = NativeType::I16Array; return true; }
    return false;
}

inline const char* nativeScalarTypeName(NativeType t) {
    switch (t) {
        case NativeType::Void: return "void";
        case NativeType::F64: return "f64";
        case NativeType::I32: return "i32";
        case NativeType::Bool: return "bool";
        case NativeType::Str: return "str";
        case NativeType::Dynamic: return "dynamic";
        case NativeType::F32Array: return "f32[]";
        case NativeType::F64Array: return "f64[]";
        case NativeType::I32Array: return "i32[]";
        case NativeType::U8Array: return "u8[]";
        case NativeType::U16Array: return "u16[]";
        case NativeType::U32Array: return "u32[]";
        case NativeType::I8Array: return "i8[]";
        case NativeType::I16Array: return "i16[]";
        case NativeType::Class: return "<class>";
    }
    return "?";
}

// The spelling a manifest carries and a signature text is built from: the
// keyword, or the class path itself.
inline std::string nativeTypeName(const NativeTypeRef& ref) {
    if (ref.kind == NativeType::Class) return ref.className;
    return nativeScalarTypeName(ref.kind);
}

// A JS path: identifiers joined by single dots — `bro`, `bro.mesh.box`,
// `bro.ai.AIAgent`. ASCII identifiers only; a native's path is a compile-time
// name and never a computed key.
inline bool isNativeJsPath(std::string_view path) {
    if (path.empty()) return false;
    bool atSegmentStart = true;
    for (char c : path) {
        if (c == '.') {
            if (atSegmentStart) return false;
            atSegmentStart = true;
            continue;
        }
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
        const bool digit = (c >= '0' && c <= '9');
        if (atSegmentStart ? !alpha : !(alpha || digit)) return false;
        atSegmentStart = false;
    }
    return !atSegmentStart;
}

// The canonical signature text a registration and a compiled module agree
// on, and what the bind step compares: `ret(p1,p2,...)`, `self` spelled for
// the implicit receiver of a method, getter or setter, typed arrays as their
// keyword, classes as their path. `bronze_native_bind` refuses a slot whose
// module-side text is not the registry's, byte for byte, so an ABI drift
// between the manifest a module was compiled against and the registration a
// host makes later is a named refusal and not a misread stack.
inline std::string nativeSignatureText(const NativeTypeRef& ret, bool hasSelf,
                                       const NativeTypeRef* params, size_t paramCount) {
    std::string out = nativeTypeName(ret);
    out += '(';
    bool first = true;
    if (hasSelf) {
        out += "self";
        first = false;
    }
    for (size_t i = 0; i < paramCount; ++i) {
        if (!first) out += ',';
        first = false;
        out += nativeTypeName(params[i]);
    }
    out += ')';
    return out;
}

}  // namespace bronze::abi
