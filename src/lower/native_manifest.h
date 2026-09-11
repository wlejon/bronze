#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "il/il.h"

namespace bronze::lower {

enum class NativeTypeKind {
    Void,
    F64,
    I32,
    Bool,
    Str,
    Dynamic, // pointers, handles, objects
};

NativeTypeKind parseNativeTypeKind(std::string_view str);
il::Type nativeTypeToIl(NativeTypeKind kind);

struct NativeFunctionSig {
    std::string symbol;
    NativeTypeKind returnType = NativeTypeKind::Void;
    std::vector<NativeTypeKind> paramTypes;

    il::Type toIlReturnType() const { return nativeTypeToIl(returnType); }
    std::vector<il::Type> toIlParamTypes() const;
};

struct NativePropertySig {
    std::string getterSymbol;
    std::string setterSymbol;
    NativeTypeKind type = NativeTypeKind::Dynamic;
};

struct NativeClassSig {
    std::string name;             // Short name, e.g. "SpatialHash3D"
    std::string qualifiedName;    // Full name, e.g. "bro.math.SpatialHash3D"
    NativeFunctionSig constructor;
    std::string destructorSymbol;
    std::unordered_map<std::string, NativeFunctionSig> methods;
    std::unordered_map<std::string, NativePropertySig> properties;
};

class NativeManifest {
public:
    NativeManifest() = default;

    static std::optional<NativeManifest> loadFromFile(const std::string& path, std::string& err);
    static std::optional<NativeManifest> loadFromDirectory(const std::string& dirPath, std::string& err);

    bool merge(const NativeManifest& other);

    // Look up function by full path e.g. "bro.math.lerp"
    const NativeFunctionSig* findFunction(const std::string& qualifiedName) const;

    // Look up class by short name ("SpatialHash3D") or qualified name ("bro.math.SpatialHash3D")
    const NativeClassSig* findClass(const std::string& nameOrQualified) const;

    // Check if an identifier is a namespace root (e.g. "bro")
    bool isNamespaceRoot(const std::string& name) const;

    // Check if an identifier is a known class name (e.g. "SpatialHash3D")
    bool isKnownClass(const std::string& name) const;

    const std::unordered_map<std::string, NativeFunctionSig>& functions() const { return functions_; }
    const std::unordered_map<std::string, NativeClassSig>& classes() const { return classes_; }
    const std::unordered_set<std::string>& namespaceRoots() const { return namespaceRoots_; }
    const std::unordered_set<std::string>& knownClasses() const { return knownClasses_; }

    const std::vector<std::string>& extraLibPaths() const { return extraLibPaths_; }
    void addExtraLibPath(const std::string& path) { extraLibPaths_.push_back(path); }

private:
    std::unordered_map<std::string, NativeFunctionSig> functions_;
    std::unordered_map<std::string, NativeClassSig> classes_;
    std::unordered_set<std::string> namespaceRoots_;
    std::unordered_set<std::string> knownClasses_;
    std::vector<std::string> extraLibPaths_;
};

}  // namespace bronze::lower
