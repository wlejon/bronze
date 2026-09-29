#pragma once

// Host natives on the embed surface: the registry a host binds C functions
// into under JS paths, the manifest an ahead-of-time build reads back off it,
// and the load-time bind of a module's import table. A companion of embed.h,
// carrying BRONZE_EMBED_API for the same reason it does; embed.h includes it,
// so a host that includes embed.h has all of it.

#include <string>
#include <string_view>
#include <vector>

#include "abi/bronze_native_type.h"  // bronze_native_buffer: a `T[]` return's out-descriptor
#include "embed/embed.h"
#include "runtime/native_handle.h"

namespace bronze::embed {

// ---- host natives (embed_native.cpp) ---------------------------------------
//
// A native is a C function the host binds under a JS path and the compiler
// lowers a call to DIRECTLY: `bro.mesh.box(w, h)` becomes a machine call with
// two doubles in registers, no dynamic dispatch, no boxing. The host globals
// (embed.h) are the dynamic half of the same boundary — a value the program
// reads and calls through the ordinary runtime paths — and the two are
// disjoint by name: a bare native path and a host global cannot share a
// name (whichever registration comes second is refused, loudly). A DOTTED
// native path under a host global's root is the normal arrangement, though:
// bro registers `bro` as an object the program can walk, and the natives are
// the paths under it the compiler short-circuits. Every OTHER access under
// that root — `let f = bro.mesh.box`, `x instanceof bro.ai.AIAgent` — goes
// through the host global as before, so a host that wants those to work
// provides them there too.
//
// Bound at LOAD time, by name, never at link time: a compiled module carries
// an import table (`<entry>_native_imports`, bronze_abi.h's loadable-module
// section) of every native it calls, the runtime fills it from this
// registry when the module is bound, and a native the module was compiled
// against that nobody registered is a refusal naming it — before the first
// call, not a crash inside one.
//
// Two ways the compiler learns the registry:
//   JIT (evalScript)      reads this thread's registry directly; register
//                         before evaluating, and the program is bound before
//                         it runs.
//   AOT (`bronze build`)  reads a manifest the registry prints:
//                         writeNativeManifest, then
//                         `bronze build --native-manifest <file>`. The
//                         module's table is bound when it is loaded
//                         (bindNativeImports), or by its entry's own first
//                         call if the loader did not.
//
// Per-thread, like the host globals, and so is a module's import table: the
// image's own table is its HOME thread's (the first to run the entry), and
// every other thread's entry binds its own copy from that thread's registry.
// bindNativeImports on the image's table therefore checks and binds only the
// home thread's; a worker's entry binds, or refuses fatally, for itself.
//
// The type vocabulary is abi/bronze_native_type.h, and it is closed: a type
// spelling not in it and not a registered class is a refused registration,
// never `dynamic`.
//
//   "void"        return only
//   "f64" "i32" "bool" "str" "dynamic"
//                 double, int32_t (ToInt32), bool, const char* (UTF-8, valid
//                 for the call only), raw Value bits. A missing scalar
//                 argument arrives as 0 / false / "".
//   "f32[]" "f64[]" "i32[]" "u8[]" "u16[]" "u32[]" "i8[]" "i16[]"
//                 a typed array of exactly that element kind, passed as TWO
//                 C parameters (T* data, uint32_t length). Wrong kind,
//                 detached buffer or missing argument: TypeError. The
//                 pointer is valid for the call only; the native must not
//                 allocate through this API while it holds it. As a RETURN
//                 type the C function returns void and takes one extra
//                 trailing `bronze_native_buffer* out`
//                 (abi/bronze_native_type.h) it fills: `release` null means
//                 the runtime COPIES `length` elements into a fresh JS-owned
//                 array, non-null means the array is a zero-copy view over
//                 `data` and `release(ctx)` runs when the buffer is collected
//                 (Deferred; at drainFinalizers). The runtime decides
//                 nothing else — no count-then-fill, no second call.
//   "<class>"     a handle made by that class's constructor, passed as the
//                 void* the constructor returned. Any other value (another
//                 class, a plain object, undefined) is a TypeError naming
//                 the class. As a return type the native returns void* and
//                 the runtime wraps it into a handle of the class (null →
//                 null), owing the class's destructor.
enum class NativeKind : uint8_t {
    Function,     // `path(args)`; params are the C parameters in order
    Method,       // `obj.member(args)`; the C function takes (void* self, args...)
    Constructor,  // `new path(args)`; returns void*; registers the class
    Getter,       // `obj.member` / `ns.prop`; C takes (void* self) / () and returns the type
    Setter,       // `obj.member = v` / `ns.prop = v`; C takes (void* self, v) / (v), returns void
};

struct NativeSignature {
    std::string returnType;               // a vocabulary spelling or a class path;
                                          // a constructor's is its own path (or empty)
    std::vector<std::string> paramTypes;  // EXCLUDING the receiver of a member kind
    NativeKind kind = NativeKind::Function;
    std::string className;                // member kinds: the class path; a
                                          // getter/setter with none is a namespace property
    std::string returnClass;              // returnType "dynamic" only: the class the value
                                          // is a handle of (the compiler tags it; the
                                          // runtime still checks at every use)
    runtime::HandleDestructor destructor = nullptr;  // Constructor only; null = the class owns nothing
    runtime::Finalize finalize = runtime::Finalize::InSweep;  // Constructor only
};

// Bind `fn` under `jsPath` (`bro.mesh.box`, `bro.ai.AIAgent.move`,
// `bro.ai.AIAgent` for a constructor, `bro.time.scale` for a namespace
// property). `fn` is called with the C signature the vocabulary above gives
// for `sig`. Returns false with `*error` set (or a fatal when `error` is
// null) for every refusal: a path that is not a JS path, an unknown type
// spelling, a class name nothing registered, a member of no class, a
// duplicate, a bare name that is already a host global. Register a class's
// constructor before anything that names the class. Does not allocate on
// the JS heap except for a constructor, which mints the class's prototype.
BRONZE_EMBED_API bool registerNative(std::string_view jsPath, void* fn, const NativeSignature& sig,
                                     std::string* error = nullptr);

// Remove one registration. A module already bound keeps calling what it
// bound; a later bind refuses the name. Unregistering a constructor unlinks
// the class: instances already made keep their tag and still answer to
// methods bound before, and no later registration can reuse that tag.
BRONZE_EMBED_API bool unregisterNative(std::string_view jsPath, NativeKind kind);

struct NativeEntry {
    std::string path;
    NativeSignature signature;
    void* fn{nullptr};
    std::string signatureText;  // the canonical "ret(params)" the bind step compares
};
// Every registration on this thread, in registration order.
BRONZE_EMBED_API std::vector<NativeEntry> hostNatives();
// The paths only, one per registration (a path registered under two kinds —
// a getter and a setter — appears twice).
BRONZE_EMBED_API std::vector<std::string> hostNativeNames();

// The prototype every instance of a registered class is born on: a plain
// object the registry roots, minted when the constructor was registered.
// This is where a host puts the JS-visible methods of the class (the
// compiled direct calls never consult it, but a dynamic `obj.move()` on a
// handle that reached the program through an untyped path does), and what
// it sets as `.prototype` of a constructor function it exposes, which is
// what makes `x instanceof bro.ai.AIAgent` answer true. `found` false for a
// name that is not a registered class.
BRONZE_EMBED_API GlobalValue nativeClassPrototype(std::string_view className);

// Wrap a host pointer as a tagged instance of a registered native class.
// The handle is born on the class's prototype and carries the class's tag,
// so native methods accept it as receiver. Returns null if data is null or
// className is not a registered native class.
BRONZE_EMBED_API Value wrapNative(void* data, std::string_view className);

// The registry as JSON — the manifest an ahead-of-time `bronze build
// --native-manifest <file>` compiles against. Deterministic for one
// registration order. writeNativeManifest is the same text to a file
// (false with `*error` set, or a fatal when `error` is null, if the file
// cannot be written).
BRONZE_EMBED_API std::string nativeManifestJson();
BRONZE_EMBED_API bool writeNativeManifest(const std::string& path, std::string* error = nullptr);

// Fill a loaded module's import table (the `<entry>_native_imports` symbol,
// resolved by the loader beside the entry) from this thread's registry.
// True when every slot was filled; otherwise `*missing` receives one line
// per slot it could not fill — "<kind> <path> (not registered)", or the
// signature mismatch — and the loader refuses the module by name. The
// module's entry calls the same bind itself and is FATAL on a gap, so this
// is the soft form for a loader that wants the refusal before entry.
// Idempotent. A JIT program (evalScript) is bound by the evaluator.
BRONZE_EMBED_API bool bindNativeImports(const void* importTable,
                                        std::vector<std::string>* missing = nullptr);

}  // namespace bronze::embed
