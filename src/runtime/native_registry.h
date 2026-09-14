#pragma once

// The native registry: what a host has bound under which JS path, per
// thread, with a checked signature each. It is the ONE source both halves of
// a native call read from — the lowerer (through the manifest the registry
// prints, or directly on the JIT path) decides at compile time that
// `bro.mesh.box(w, h)` is a direct call with two doubles, and the bind step
// fills the module's import slot for `function bro.mesh.box` from the entry
// registered here, at LOAD time, by name. No native symbol is ever resolved
// by a linker.
//
// Registration is checked, and the checks are the contract embed.h
// documents for registerNative: a path must be a JS path, a type must be in
// the vocabulary (abi/bronze_native_type.h) or name a class already
// registered, a member must belong to a registered class, a bare name must
// not already be a host global. Every refusal is a returned message, never a
// silent default.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "abi/bronze_native_type.h"
#include "runtime/native_handle.h"
#include "runtime/value.h"

namespace bronze::runtime {

enum class NativeKind : uint8_t { Function, Method, Constructor, Getter, Setter };

const char* nativeKindName(NativeKind kind);
bool parseNativeKind(std::string_view text, NativeKind& out);

// A registered class. Its ADDRESS is the class tag every handle of the class
// carries (native_handle.h), so an info is never freed or moved for the life
// of the thread: unregistering the class only unlinks it, and the instances
// already made keep answering to a tag no new registration can reuse.
struct NativeClassInfo {
    std::string name;  // the class path, e.g. "bro.ai.AIAgent"
    HandleDestructor destructor = nullptr;
    Finalize finalize = Finalize::InSweep;
    // The prototype every instance is born on: a plain object the registry
    // roots. The host reads it out (embed::nativeClassPrototype) to install
    // JS-visible methods or to make it its constructor's `.prototype`, which
    // is what makes `instanceof` answer.
    Value prototype = Value::fromUndefined();
    bool live = true;
};

// What a host hands registerNative, in the vocabulary's spellings.
struct NativeSpec {
    NativeKind kind = NativeKind::Function;
    std::string returnType;               // "void", "f64", ..., or a class path
    std::vector<std::string> paramTypes;  // excludes the receiver
    std::string className;                // member kinds: the class path
    std::string returnClass;              // with returnType "dynamic": the class the value is known to be
    HandleDestructor destructor = nullptr;
    Finalize finalize = Finalize::InSweep;
};

// A registration as the registry holds it: the spec resolved to type refs,
// plus the function pointer and the class (for a constructor).
struct NativeEntry {
    std::string path;
    NativeKind kind = NativeKind::Function;
    std::string className;
    abi::NativeTypeRef returnType;
    std::vector<abi::NativeTypeRef> paramTypes;
    std::string returnClass;
    void* fn = nullptr;
    NativeClassInfo* classInfo = nullptr;  // Constructor only
    // "ret(params)" with `self` for the receiver of a member kind — the text
    // the manifest carries and the bind step compares.
    std::string signature;
};

// The import name a compiled module records for one slot: "<kind> <path>",
// or "class <path>" for a class-tag slot.
std::string nativeImportName(NativeKind kind, std::string_view path);

// false with `err` set on any refusal; the registry is unchanged then.
bool rtRegisterNative(std::string_view path, void* fn, const NativeSpec& spec, std::string& err);
// true if something was removed. Unregistering a constructor unlinks the
// class; its members stay registered but no longer resolve at bind.
bool rtUnregisterNative(std::string_view path, NativeKind kind);
const std::vector<NativeEntry>& rtNativeEntries();
const NativeEntry* rtFindNative(std::string_view path, NativeKind kind);
NativeClassInfo* rtFindNativeClass(std::string_view name);
// Is `name` a bare (undotted) native path? rtRegisterHostGlobal asks, and
// refuses a host global of that name.
bool rtNativeBareNameRegistered(std::string_view name);

// Fill a module's import table from this thread's registry. `missing`
// receives one line per slot that could not be filled — an unregistered
// native, or one registered with a different signature — and the return is
// whether every slot was filled. A slot that IS filled is written whether or
// not the others are, so a partial bind followed by a fix and a rebind is
// well-defined. Layout: bronze_abi.h's loadable-module section.
bool rtBindNativeImports(const void* table, std::vector<std::string>* missing);

}  // namespace bronze::runtime
