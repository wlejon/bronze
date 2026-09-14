#pragma once

// The native manifest as the LOWERER sees it: which JS paths are direct
// native calls, with what C signature. It is the host's registry
// (runtime/native_registry.h) read back — directly, on the JIT path, or
// through the JSON `embed::writeNativeManifest` prints for an ahead-of-time
// `bronze build --native-manifest <file>`. One reader for both: the JIT path
// serializes the registry to the same JSON and parses it here, so the two
// cannot disagree about what a manifest means.
//
// Everything a manifest says is CHECKED here, at parse: a kind, a type
// spelling, a class reference, a member's class — each either resolves or
// refuses the whole manifest with a message naming the entry. There is no
// default type. The form is the one the writer prints (embed_native.cpp):
//
//   { "version": 1,
//     "natives": [
//       { "path": "bro.mesh.box", "kind": "function",
//         "returns": "f64", "params": ["f64", "i32"], "signature": "f64(f64,i32)" },
//       { "path": "bro.ai.AIAgent", "kind": "constructor", "class": "bro.ai.AIAgent",
//         "returns": "bro.ai.AIAgent", "params": [], "signature": "bro.ai.AIAgent()" },
//       { "path": "bro.ai.AIAgent.move", "kind": "method", "class": "bro.ai.AIAgent",
//         "returns": "void", "params": ["f64"], "signature": "void(self,f64)" },
//       { "path": "bro.time.scale", "kind": "getter", "returns": "f64", "params": [],
//         "signature": "f64()" } ] }
//
// The prototype's `namespaces` / `classes`-object / `symbols` forms are
// refused by name: a manifest in one of them is an old file, not a partial
// new one.

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "abi/bronze_native_type.h"
#include "il/il.h"

namespace bronze::lower {

enum class NativeKind : uint8_t { Function, Method, Constructor, Getter, Setter };

const char* nativeKindName(NativeKind kind);

struct NativeSig {
    std::string path;
    NativeKind kind = NativeKind::Function;
    std::string className;   // member kinds and constructors
    abi::NativeTypeRef returnType;
    std::vector<abi::NativeTypeRef> paramTypes;  // excluding the receiver
    std::string returnClass;  // returnType dynamic: the class the value is tagged as
    std::string signature;    // canonical text (abi/bronze_native_type.h)

    bool hasSelf() const {
        return kind == NativeKind::Method ||
               ((kind == NativeKind::Getter || kind == NativeKind::Setter) && !className.empty());
    }
    // The class a value produced by this native is known to be, or empty.
    const std::string& producedClass() const {
        return returnType.kind == abi::NativeType::Class ? returnType.className : returnClass;
    }
};

class NativeManifest {
public:
    NativeManifest() = default;

    // `origin` names the text in messages (a path, or "<registry>").
    static std::optional<NativeManifest> parse(std::string_view json, std::string_view origin,
                                               std::string& err);
    static std::optional<NativeManifest> loadFromFile(const std::string& path, std::string& err);
    // Every `*.json` in the directory, merged; a path or kind in two files
    // is an error, not a silent override.
    static std::optional<NativeManifest> loadFromDirectory(const std::string& dirPath,
                                                           std::string& err);

    bool empty() const { return entries_.empty(); }
    const std::vector<NativeSig>& entries() const { return entries_; }

    const NativeSig* findFunction(std::string_view path) const;
    const NativeSig* findConstructor(std::string_view classPath) const;
    const NativeSig* findMethod(std::string_view classPath, std::string_view member) const;
    // `owner` is a class path (an instance property) or a namespace path.
    const NativeSig* findGetter(std::string_view owner, std::string_view member) const;
    const NativeSig* findSetter(std::string_view owner, std::string_view member) const;
    bool isClass(std::string_view path) const { return classes_.contains(std::string(path)); }

    // The first segment of every dotted path — `bro` for `bro.mesh.box` —
    // which is what a program mentions as a free identifier. Each joins the
    // provided-globals set so that the accesses the lowering does NOT
    // short-circuit (`bro.mesh` as a value) resolve like any host global's.
    const std::vector<std::string>& namespaceRoots() const { return roots_; }

private:
    bool add(NativeSig sig, std::string& err);
    static std::string key(NativeKind kind, std::string_view path);

    std::vector<NativeSig> entries_;
    std::unordered_map<std::string, size_t> index_;  // key() -> entries_ index
    std::unordered_set<std::string> classes_;
    std::vector<std::string> roots_;
};

}  // namespace bronze::lower
