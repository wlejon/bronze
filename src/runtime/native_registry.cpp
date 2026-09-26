#include "runtime/native_registry.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/external_store.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/host_globals.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/typed_array.h"

namespace bronze::runtime {

namespace {

// Per thread, like the host globals: a registration is a fact about the
// thread that made it, and the module epochs and root spans are per thread
// too. The class infos are owned here and never freed (their address is the
// tag; see the header).
thread_local std::vector<NativeEntry> g_natives;
thread_local std::vector<std::unique_ptr<NativeClassInfo>> g_classes;

void ensureClassRoots() {
    static thread_local const bool registered = [] {
        rtHeap().add_root_source([](const Heap::RootVisitor& visit) {
            for (auto& cls : g_classes) visit(cls->prototype);
        });
        return true;
    }();
    (void)registered;
}

bool resolveType(std::string_view text, bool asParam, abi::NativeTypeRef& out, std::string& err) {
    abi::NativeType scalar;
    if (abi::parseNativeScalarType(text, scalar)) {
        if (asParam && scalar == abi::NativeType::Void) {
            err = "a parameter cannot be 'void'";
            return false;
        }
        out.kind = scalar;
        out.className.clear();
        return true;
    }
    if (NativeClassInfo* cls = rtFindNativeClass(text)) {
        out.kind = abi::NativeType::Class;
        out.className = cls->name;
        return true;
    }
    err = "unknown type '" + std::string(text) +
          "': not one of void, f64, i32, bool, str, dynamic, f32[], f64[], i32[], u8[], "
          "u16[], u32[], i8[], i16[], and no native class of that name is registered "
          "(register a class's constructor before anything that names it)";
    return false;
}

}  // namespace

const char* nativeKindName(NativeKind kind) {
    switch (kind) {
        case NativeKind::Function: return "function";
        case NativeKind::Method: return "method";
        case NativeKind::Constructor: return "constructor";
        case NativeKind::Getter: return "getter";
        case NativeKind::Setter: return "setter";
    }
    return "?";
}

bool parseNativeKind(std::string_view text, NativeKind& out) {
    if (text == "function") { out = NativeKind::Function; return true; }
    if (text == "method") { out = NativeKind::Method; return true; }
    if (text == "constructor") { out = NativeKind::Constructor; return true; }
    if (text == "getter") { out = NativeKind::Getter; return true; }
    if (text == "setter") { out = NativeKind::Setter; return true; }
    return false;
}

std::string nativeImportName(NativeKind kind, std::string_view path) {
    return std::string(nativeKindName(kind)) + " " + std::string(path);
}

const std::vector<NativeEntry>& rtNativeEntries() { return g_natives; }

const NativeEntry* rtFindNative(std::string_view path, NativeKind kind) {
    for (const auto& e : g_natives) {
        if (e.kind == kind && e.path == path) return &e;
    }
    return nullptr;
}

NativeClassInfo* rtFindNativeClass(std::string_view name) {
    for (auto& cls : g_classes) {
        if (cls->live && cls->name == name) return cls.get();
    }
    return nullptr;
}

bool rtNativeBareNameRegistered(std::string_view name) {
    for (const auto& e : g_natives) {
        if (e.path == name && e.path.find('.') == std::string::npos) return true;
    }
    return false;
}

bool rtRegisterNative(std::string_view pathView, void* fn, const NativeSpec& spec, std::string& err) {
    const std::string path(pathView);
    auto refuse = [&](const std::string& why) {
        err = "registerNative(\"" + path + "\", " + nativeKindName(spec.kind) + "): " + why;
        return false;
    };
    if (!abi::isNativeJsPath(path)) {
        return refuse("not a JS path (identifiers joined by single dots)");
    }
    if (!fn) return refuse("null function pointer");
    if (rtFindNative(path, spec.kind)) return refuse("already registered");

    NativeEntry entry;
    entry.path = path;
    entry.kind = spec.kind;
    entry.fn = fn;
    entry.returnClass = spec.returnClass;

    const bool isMember = spec.kind == NativeKind::Method ||
                          ((spec.kind == NativeKind::Getter || spec.kind == NativeKind::Setter) &&
                           !spec.className.empty());

    // The class relationships first, because the types below may name the
    // class this registration is creating.
    NativeClassInfo* ownClass = nullptr;
    if (spec.kind == NativeKind::Constructor) {
        if (!spec.className.empty() && spec.className != path) {
            return refuse("a constructor's className must be its own path (or empty)");
        }
        if (rtFindNativeClass(path)) return refuse("class already registered");
        for (const auto& e : g_natives) {
            if (e.path == path) {
                return refuse("the path is already a " + std::string(nativeKindName(e.kind)));
            }
        }
        if (!spec.returnType.empty() && spec.returnType != path) {
            return refuse("a constructor returns its own class: returnType must be '" + path +
                          "' or empty");
        }
        entry.className = path;
    } else {
        if (rtFindNativeClass(path)) return refuse("the path is a registered class");
        if (isMember) {
            NativeClassInfo* cls = rtFindNativeClass(spec.className);
            if (!cls) {
                return refuse("className '" + spec.className +
                              "' is not a registered class (register its constructor first)");
            }
            const std::string prefix = cls->name + ".";
            if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0 ||
                path.find('.', prefix.size()) != std::string::npos) {
                return refuse("a member's path must be '" + cls->name + ".<member>'");
            }
            entry.className = cls->name;
        } else if (spec.kind == NativeKind::Method) {
            return refuse("a method needs a className");
        } else if (!spec.className.empty()) {
            return refuse("className is only for method, getter, setter and constructor");
        } else if ((spec.kind == NativeKind::Getter || spec.kind == NativeKind::Setter) &&
                   path.find('.') == std::string::npos) {
            return refuse("a namespace property needs a dotted path (a bare identifier is not a "
                          "property access)");
        }
    }

    // Provisionally link the class so its own members and return type can
    // name it — undone below on any later refusal.
    std::unique_ptr<NativeClassInfo> pendingClass;
    if (spec.kind == NativeKind::Constructor) {
        pendingClass = std::make_unique<NativeClassInfo>();
        pendingClass->name = path;
        pendingClass->destructor = spec.destructor;
        pendingClass->finalize = spec.finalize;
        ownClass = pendingClass.get();
        g_classes.push_back(std::move(pendingClass));
    }
    auto unlinkOwnClass = [&]() {
        if (ownClass) {
            g_classes.pop_back();
            ownClass = nullptr;
        }
    };

    std::string typeErr;
    if (spec.kind == NativeKind::Constructor) {
        entry.returnType.kind = abi::NativeType::Class;
        entry.returnType.className = path;
    } else if (!resolveType(spec.returnType, /*asParam=*/false, entry.returnType, typeErr)) {
        unlinkOwnClass();
        return refuse("returnType: " + typeErr);
    }
    for (const auto& p : spec.paramTypes) {
        abi::NativeTypeRef ref;
        if (!resolveType(p, /*asParam=*/true, ref, typeErr)) {
            unlinkOwnClass();
            return refuse("paramTypes: " + typeErr);
        }
        entry.paramTypes.push_back(std::move(ref));
    }
    if (!spec.returnClass.empty()) {
        if (entry.returnType.kind != abi::NativeType::Dynamic) {
            unlinkOwnClass();
            return refuse("returnClass is only for returnType 'dynamic' (a value the native "
                          "already made a handle of); a native returning a raw pointer names "
                          "the class as its returnType");
        }
        if (!rtFindNativeClass(spec.returnClass)) {
            unlinkOwnClass();
            return refuse("returnClass '" + spec.returnClass + "' is not a registered class");
        }
    }
    if (spec.kind == NativeKind::Getter) {
        if (!entry.paramTypes.empty()) {
            unlinkOwnClass();
            return refuse("a getter takes no parameters");
        }
        if (entry.returnType.kind == abi::NativeType::Void) {
            unlinkOwnClass();
            return refuse("a getter returns a value");
        }
    }
    if (spec.kind == NativeKind::Setter) {
        if (entry.paramTypes.size() != 1) {
            unlinkOwnClass();
            return refuse("a setter takes exactly one parameter");
        }
        if (entry.returnType.kind != abi::NativeType::Void) {
            unlinkOwnClass();
            return refuse("a setter returns void");
        }
    }
    if (spec.kind == NativeKind::Function && spec.destructor) {
        unlinkOwnClass();
        return refuse("a destructor belongs on a constructor");
    }

    // A bare name is what a program reads as a free identifier, which is the
    // host globals' namespace: the same name in both would make `f(1)` a
    // native call and `f` a host value, and the two would drift.
    if (path.find('.') == std::string::npos) {
        Value ignored = Value::fromUndefined();
        if (rtHostGlobalLookup(path, ignored)) {
            unlinkOwnClass();
            return refuse("'" + path + "' is a registered host global; a name is a native or a "
                          "host global, not both");
        }
    }

    entry.signature = abi::nativeSignatureText(entry.returnType, isMember, entry.paramTypes.data(),
                                               entry.paramTypes.size());
    if (ownClass) {
        ensureClassRoots();
        ShadowStackFrame frame;
        ObjectHeader* proto = ObjectHeader::create(rtHeap(), rtArena(), rtPlainObjectShape());
        proto->header.flags = HeapKind::Plain;
        ownClass->prototype = Value::fromObject(proto);
        entry.classInfo = ownClass;
    }
    g_natives.push_back(std::move(entry));
    return true;
}

bool rtUnregisterNative(std::string_view path, NativeKind kind) {
    for (size_t i = 0; i < g_natives.size(); ++i) {
        if (g_natives[i].kind == kind && g_natives[i].path == path) {
            if (g_natives[i].classInfo) g_natives[i].classInfo->live = false;
            g_natives.erase(g_natives.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

// ---- the import table ------------------------------------------------------
//
//   uint32_t count;
//   uint32_t namesOffset;      // from the table's first byte
//   uint64_t slots[count];     // at +8: function pointers, or a class tag
//   char     names[];          // at namesOffset: count × ("name\0" "sig\0")
//
// The name is "<kind> <path>" or "class <path>"; the signature is the
// canonical text (abi/bronze_native_type.h), empty for a class slot.

namespace {

struct ImportTableView {
    uint32_t count = 0;
    uint64_t* slots = nullptr;
    const char* names = nullptr;
};

ImportTableView viewImportTable(const void* table) {
    ImportTableView v;
    if (!table) return v;
    const auto* bytes = static_cast<const unsigned char*>(table);
    uint32_t namesOffset = 0;
    std::memcpy(&v.count, bytes, sizeof(uint32_t));
    std::memcpy(&namesOffset, bytes + sizeof(uint32_t), sizeof(uint32_t));
    v.slots = reinterpret_cast<uint64_t*>(const_cast<unsigned char*>(bytes) + 8);
    v.names = reinterpret_cast<const char*>(bytes + namesOffset);
    return v;
}

}  // namespace

bool rtBindNativeImports(const void* table, std::vector<std::string>* missing) {
    ImportTableView v = viewImportTable(table);
    bool complete = true;
    const char* cursor = v.names;
    for (uint32_t i = 0; i < v.count; ++i) {
        const std::string name(cursor);
        cursor += name.size() + 1;
        const std::string signature(cursor);
        cursor += signature.size() + 1;

        const size_t space = name.find(' ');
        const std::string kindText = name.substr(0, space);
        const std::string path = (space == std::string::npos) ? std::string() : name.substr(space + 1);
        if (kindText == "class") {
            if (NativeClassInfo* cls = rtFindNativeClass(path)) {
                v.slots[i] = reinterpret_cast<uint64_t>(cls);
                continue;
            }
            complete = false;
            if (missing) missing->push_back(name + " (no class of that name is registered)");
            continue;
        }
        NativeKind kind;
        if (!parseNativeKind(kindText, kind)) {
            complete = false;
            if (missing) missing->push_back(name + " (unreadable import name)");
            continue;
        }
        const NativeEntry* entry = rtFindNative(path, kind);
        if (!entry) {
            complete = false;
            if (missing) missing->push_back(name + " (not registered)");
            continue;
        }
        if (entry->signature != signature) {
            complete = false;
            if (missing) {
                missing->push_back(name + " (module compiled against " + signature +
                                   ", registered as " + entry->signature + ")");
            }
            continue;
        }
        v.slots[i] = reinterpret_cast<uint64_t>(entry->fn);
    }
    return complete;
}

}  // namespace bronze::runtime

// ---- the ABI helpers generated code calls ---------------------------------

using namespace bronze;
using namespace bronze::runtime;

extern "C" {

void bronze_native_bind(uint64_t* table) {
    std::vector<std::string> missing;
    if (rtBindNativeImports(table, &missing)) return;
    std::string msg = "native imports unbound at module entry — the host registered no native "
                      "for:";
    for (const auto& m : missing) {
        msg += "\n  ";
        msg += m;
    }
    fatal(msg.c_str());
}

void bronze_native_unbound() {
    fatal("a native was called through an import slot that was never bound (the module's entry "
          "did not run, or the table was bound on another thread's registry)");
}

void* bronze_native_handle_data(uint64_t value, const void* classInfo) {
    const auto* cls = static_cast<const NativeClassInfo*>(classInfo);
    if (!cls) fatal("native class tag unbound at a call site");
    const Value v{value};
    if (rtHandleClassTag(v) == classInfo) return rtHandleData(v);
    // One message per way to be wrong, because "expected X" alone sends a
    // caller looking for a missing argument when they passed a handle of the
    // wrong class.
    std::string what;
    if (rtIsHandle(v)) {
        const auto* other = static_cast<const NativeClassInfo*>(rtHandleClassTag(v));
        what = other ? ("a " + other->name + " handle") : "an untagged native handle";
    } else if (v.isUndefined()) {
        what = "undefined (missing argument)";
    } else if (v.isNull()) {
        what = "null";
    } else if (v.isObject()) {
        what = "an ordinary object";
    } else {
        what = "a non-object";
    }
    rtThrowTypeError("expected a " + cls->name + " handle, got " + what);
    return nullptr;
}

uint64_t bronze_native_wrap(void* data, const void* classInfo) {
    const auto* cls = static_cast<const NativeClassInfo*>(classInfo);
    if (!cls) fatal("native class tag unbound at a call site");
    if (!data) return Value::fromNull().rawBits();
    return rtMakeHandle(data, cls->destructor, cls->finalize, cls->prototype, classInfo).rawBits();
}

void* bronze_native_typed_array_data(uint64_t value, uint32_t kind) {
    const Value v{value};
    const auto wanted = static_cast<ElementKind>(kind);
    const char* wantedName = elementKindInfo(wanted).name;
    if (!v.isObject() || v.asObject<HeapObjectHeader>()->flags != HeapKind::TypedArray) {
        std::string what = v.isUndefined() ? "undefined (missing argument)"
                           : v.isNull()    ? "null"
                           : v.isObject()  ? "a non-typed-array object"
                                           : "a non-object";
        rtThrowTypeError(std::string("expected a ") + wantedName + ", got " + what);
    }
    auto* view = v.asObject<TypedArrayHeader>();
    if (view->elementKind() != wanted) {
        rtThrowTypeError(std::string("expected a ") + wantedName + ", got a " + view->kindName());
    }
    if (view->buffer.asObject<ArrayBufferHeader>()->isDetached()) {
        rtThrowTypeError(std::string("the ") + wantedName + " argument's buffer is detached");
    }
    return view->bytes();
}

uint32_t bronze_native_typed_array_length(uint64_t value) {
    const Value v{value};
    if (!v.isObject() || v.asObject<HeapObjectHeader>()->flags != HeapKind::TypedArray) return 0;
    return v.asObject<TypedArrayHeader>()->length;
}

// The per-thread scratch a `str` argument is copied into. A stack, because
// native calls nest (a native that re-enters the program, which calls a
// native): each call pushes its arguments' copies and pops that many after
// the native returns, so an outer call's text is never under an inner
// call's pop. Entries are std::strings held by value, so a push never moves
// the bytes an earlier entry handed out.
thread_local std::vector<std::unique_ptr<std::string>> g_strScratch;

const void* bronze_native_str_utf8(uint64_t value) {
    const Value v{value};
    std::string text;
    if (v.isString()) {
        text = rtUtf8Chars(v.asString<StringHeader>());
    } else if (!v.isUndefined()) {
        // ToString of a non-string ALLOCATES (and can throw). The lowering
        // runs every str conversion before it takes any typed-array pointer,
        // so the move that may follow is harmless here.
        ShadowStackFrame frame;
        Rooted<Value> str{rtValueToString(v)};
        text = rtUtf8Chars(str.get().asString<StringHeader>());
    }
    g_strScratch.push_back(std::make_unique<std::string>(std::move(text)));
    return g_strScratch.back()->c_str();
}

void bronze_native_str_release(uint32_t count) {
    const size_t n = std::min<size_t>(count, g_strScratch.size());
    g_strScratch.resize(g_strScratch.size() - n);
}

uint64_t bronze_native_str_from_utf8(const void* utf8) {
    if (!utf8) return rtMakeString(std::string_view{}).rawBits();
    return rtMakeString(std::string_view{static_cast<const char*>(utf8)}).rawBits();
}

// The per-thread descriptors a `T[]`-returning native fills. A stack, for
// the reason the str scratch is one: a native may re-enter the program, which
// may reach another buffer-returning native before the outer wrap reads its
// slot. The thunk pushes with slot() and pops with wrap() on return or with
// abandon() from its landing pad when the native throws, so the slots nest
// exactly as the thunk frames do. Each slot records its thunk's frame (the
// return-address slot of the helper call, the same for every helper one
// thunk call makes), which the pop checks. A deque, so a push never moves the
// descriptor an outer call handed its native.
struct BufferSlot {
    bronze_native_buffer desc;
    uintptr_t frame;
};
thread_local std::deque<BufferSlot> g_bufferSlots;

namespace {

#if defined(_MSC_VER)
#define BRONZE_THUNK_FRAME() reinterpret_cast<uintptr_t>(_AddressOfReturnAddress())
#else
#define BRONZE_THUNK_FRAME() reinterpret_cast<uintptr_t>(__builtin_frame_address(0))
#endif

bronze_native_buffer popSlot(uintptr_t thunkFrame, const char* helper) {
    if (g_bufferSlots.empty() || g_bufferSlots.back().frame != thunkFrame) {
        fatal((std::string(helper) + " without a matching bronze_native_buffer_slot").c_str());
    }
    const bronze_native_buffer desc = g_bufferSlots.back().desc;
    g_bufferSlots.pop_back();
    return desc;
}

}  // namespace

void* bronze_native_buffer_slot() {
    g_bufferSlots.push_back(
        BufferSlot{bronze_native_buffer{nullptr, 0, nullptr, nullptr}, BRONZE_THUNK_FRAME()});
    return &g_bufferSlots.back().desc;
}

void bronze_native_buffer_abandon() {
    // The native threw: a block it had already given away is released here,
    // since no buffer will ever own it.
    const bronze_native_buffer desc = popSlot(BRONZE_THUNK_FRAME(), "bronze_native_buffer_abandon");
    if (desc.release) desc.release(desc.ctx);
}

namespace {

// The transfer mode's deleter: the native's release, with its ctx, called
// once the JS buffer dies. `user` carries the release pointer, `bytes` is
// unused because the native's ctx names its own block.
struct TransferRelease {
    void (*release)(void* ctx);
    void* ctx;
};

void runTransferRelease(void* user, uint8_t* bytes) {
    (void)bytes;
    auto* tr = static_cast<TransferRelease*>(user);
    tr->release(tr->ctx);
    delete tr;
}

}  // namespace

uint64_t bronze_native_buffer_wrap(uint32_t kind) {
    const bronze_native_buffer desc = popSlot(BRONZE_THUNK_FRAME(), "bronze_native_buffer_wrap");
    // A filled descriptor whose wrap cannot happen still owes its release:
    // the block was given away the moment the native set `release`.
    auto releaseNow = [&] {
        if (desc.release) desc.release(desc.ctx);
    };
    const auto elementKind = static_cast<ElementKind>(kind);
    const uint64_t bpe = elementKindInfo(elementKind).bytesPerElement;
    const uint64_t byteLength = static_cast<uint64_t>(desc.length) * bpe;
    ShadowStackFrame frame;
    if (byteLength > kMaxByteLength) {
        releaseNow();
        return rtThrowRangeError(std::string("a native returned a ") +
                                 elementKindInfo(elementKind).name + " of " +
                                 std::to_string(desc.length) + " elements, over the buffer maximum")
            .rawBits();
    }
    if (!desc.data || desc.length == 0) {
        // Empty in either mode; a transferred empty block is released at
        // once, since no buffer will ever own it.
        releaseNow();
        return rtNewTypedArray(elementKind, 0).rawBits();
    }
    if (!desc.release) {
        // Copy mode: the native's pointer is valid for this call only.
        Rooted<Value> view{rtNewTypedArray(elementKind, desc.length)};
        std::memcpy(view.get().asObject<TypedArrayHeader>()->bytes(), desc.data,
                    static_cast<size_t>(byteLength));
        return view.get().rawBits();
    }
    // Transfer mode: a buffer over the native's block, owing release(ctx).
    auto* tr = new TransferRelease{desc.release, desc.ctx};
    Rooted<Value> buffer{Value::fromUndefined()};
    Value refusal;
    if (rtTryCatch([&] {
            buffer.set(rtCreateExternalArrayBuffer(static_cast<uint8_t*>(desc.data),
                                                   static_cast<uint32_t>(byteLength),
                                                   runTransferRelease, tr));
        }, refusal)) {
        // Refused before any registration owned the block (the length ladder
        // above already ran, so this is a refusal the store itself made).
        Rooted<Value> thrown{refusal};
        delete tr;
        releaseNow();
        rtThrow(thrown.get());
    }
    return rtNewTypedArrayOverBuffer(elementKind, buffer, 0, desc.length, /*tracking=*/false)
        .rawBits();
}

}  // extern "C"
