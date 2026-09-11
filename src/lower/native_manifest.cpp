#include "lower/native_manifest.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "json/json.h"

namespace bronze::lower {

namespace {

json::Units toUnits(std::string_view utf8) {
    json::Units out;
    size_t i = 0;
    while (i < utf8.size()) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        uint32_t cp = 0;
        size_t extra = 0;
        if (c < 0x80) {
            cp = c;
            extra = 0;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1Fu;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0Fu;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07u;
            extra = 3;
        } else {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        if (i + extra >= utf8.size()) {
            out.push_back(0xFFFD);
            break;
        }
        bool ok = true;
        for (size_t k = 1; k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(utf8[i + k]);
            if ((cc & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (!ok) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        i += extra + 1;
        if (cp > 0x10FFFF) {
            out.push_back(0xFFFD);
            continue;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

std::string toUtf8(const json::Units& units) {
    std::string out;
    for (size_t i = 0; i < units.size(); ++i) {
        uint32_t cp = units[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < units.size() && units[i + 1] >= 0xDC00 &&
            units[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (units[++i] - 0xDC00);
        }
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

const json::Value* findMember(const json::Value* obj, std::string_view key) {
    if (!obj || obj->kind != json::Value::Kind::Object) return nullptr;
    for (const auto& m : obj->members) {
        if (toUtf8(m.key) == key) return m.value.get();
    }
    return nullptr;
}

std::string getStringMember(const json::Value* obj, std::string_view key) {
    const auto* v = findMember(obj, key);
    if (!v || v->kind != json::Value::Kind::String) return "";
    return toUtf8(v->text);
}

}  // namespace

NativeTypeKind parseNativeTypeKind(std::string_view str) {
    if (str == "void") return NativeTypeKind::Void;
    if (str == "f64" || str == "number" || str == "float" || str == "double") return NativeTypeKind::F64;
    if (str == "i32" || str == "int" || str == "int32") return NativeTypeKind::I32;
    if (str == "bool" || str == "boolean") return NativeTypeKind::Bool;
    if (str == "str" || str == "string") return NativeTypeKind::Str;
    return NativeTypeKind::Dynamic;
}

il::Type nativeTypeToIl(NativeTypeKind kind) {
    switch (kind) {
        case NativeTypeKind::Void: return il::Type::Void;
        case NativeTypeKind::F64: return il::Type::F64;
        case NativeTypeKind::I32: return il::Type::I32;
        case NativeTypeKind::Bool: return il::Type::Bool;
        case NativeTypeKind::Str: return il::Type::Str;
        case NativeTypeKind::Dynamic: return il::Type::Dynamic;
    }
    return il::Type::Dynamic;
}

std::vector<il::Type> NativeFunctionSig::toIlParamTypes() const {
    std::vector<il::Type> result;
    result.reserve(paramTypes.size());
    for (auto pt : paramTypes) {
        result.push_back(nativeTypeToIl(pt));
    }
    return result;
}

std::optional<NativeManifest> NativeManifest::loadFromFile(const std::string& path, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "Cannot open native manifest file: " + path;
        return std::nullopt;
    }

    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();

    std::string jsonErr;
    json::ValuePtr root = json::parse(toUnits(text), jsonErr);
    if (!root) {
        err = "Invalid JSON in native manifest '" + path + "': " + jsonErr;
        return std::nullopt;
    }

    NativeManifest manifest;

    // 1. Parse namespaces
    if (const auto* nsObj = findMember(root.get(), "namespaces")) {
        if (nsObj->kind == json::Value::Kind::Object) {
            for (const auto& nsMember : nsObj->members) {
                std::string nsName = toUtf8(nsMember.key);
                if (nsName.empty()) continue;

                // Extract root namespace identifier (e.g. "bro" from "bro.math")
                auto dotPos = nsName.find('.');
                std::string rootNs = (dotPos != std::string::npos) ? nsName.substr(0, dotPos) : nsName;
                manifest.namespaceRoots_.insert(rootNs);

                const auto* nsVal = nsMember.value.get();
                if (const auto* funcsObj = findMember(nsVal, "functions")) {
                    if (funcsObj->kind == json::Value::Kind::Object) {
                        for (const auto& fnMember : funcsObj->members) {
                            std::string fnName = toUtf8(fnMember.key);
                            const auto* fnVal = fnMember.value.get();
                            if (!fnVal) continue;

                            NativeFunctionSig sig;
                            sig.symbol = getStringMember(fnVal, "symbol");
                            sig.returnType = parseNativeTypeKind(getStringMember(fnVal, "returnType"));
                            sig.returnClass = getStringMember(fnVal, "returnClass");

                            if (const auto* paramsArr = findMember(fnVal, "paramTypes")) {
                                if (paramsArr->kind == json::Value::Kind::Array) {
                                    for (const auto& pElem : paramsArr->elements) {
                                        if (pElem && pElem->kind == json::Value::Kind::String) {
                                            sig.paramTypes.push_back(parseNativeTypeKind(toUtf8(pElem->text)));
                                        }
                                    }
                                }
                            }

                            if (!sig.symbol.empty()) {
                                std::string qName = nsName + "." + fnName;
                                manifest.functions_[qName] = sig;
                            }
                        }
                    }
                }

                if (const auto* propsObj = findMember(nsVal, "properties")) {
                    if (propsObj->kind == json::Value::Kind::Object) {
                        for (const auto& pMember : propsObj->members) {
                            std::string propName = toUtf8(pMember.key);
                            const auto* pVal = pMember.value.get();
                            if (!pVal) continue;

                            NativePropertySig psig;
                            psig.getterSymbol = getStringMember(pVal, "getter");
                            psig.setterSymbol = getStringMember(pVal, "setter");
                            psig.type = parseNativeTypeKind(getStringMember(pVal, "returnType"));

                            std::string qName = nsName + "." + propName;
                            manifest.namespaceProperties_[qName] = psig;
                        }
                    }
                }
            }
        }
    }

    // 2. Parse classes
    if (const auto* classesObj = findMember(root.get(), "classes")) {
        if (classesObj->kind == json::Value::Kind::Object) {
            for (const auto& clsMember : classesObj->members) {
                std::string clsKey = toUtf8(clsMember.key);
                const auto* clsVal = clsMember.value.get();
                if (!clsVal) continue;

                NativeClassSig clsSig;
                clsSig.qualifiedName = clsKey;
                clsSig.name = getStringMember(clsVal, "name");
                if (clsSig.name.empty()) {
                    auto dotPos = clsKey.rfind('.');
                    clsSig.name = (dotPos != std::string::npos) ? clsKey.substr(dotPos + 1) : clsKey;
                }

                // Constructor
                if (const auto* ctorVal = findMember(clsVal, "constructor")) {
                    clsSig.constructor.symbol = getStringMember(ctorVal, "symbol");
                    clsSig.constructor.returnType = parseNativeTypeKind(getStringMember(ctorVal, "returnType"));
                    clsSig.constructor.returnClass = getStringMember(ctorVal, "returnClass");
                    if (clsSig.constructor.returnClass.empty()) {
                        clsSig.constructor.returnClass = clsSig.name;
                    }
                    if (const auto* paramsArr = findMember(ctorVal, "paramTypes")) {
                        if (paramsArr->kind == json::Value::Kind::Array) {
                            for (const auto& pElem : paramsArr->elements) {
                                if (pElem && pElem->kind == json::Value::Kind::String) {
                                    clsSig.constructor.paramTypes.push_back(parseNativeTypeKind(toUtf8(pElem->text)));
                                }
                            }
                        }
                    }
                }

                // Destructor
                if (const auto* dtorVal = findMember(clsVal, "destructor")) {
                    clsSig.destructorSymbol = getStringMember(dtorVal, "symbol");
                }

                // Methods
                if (const auto* methodsObj = findMember(clsVal, "methods")) {
                    if (methodsObj->kind == json::Value::Kind::Object) {
                        for (const auto& mMember : methodsObj->members) {
                            std::string mName = toUtf8(mMember.key);
                            const auto* mVal = mMember.value.get();
                            if (!mVal) continue;

                            NativeFunctionSig msig;
                            msig.symbol = getStringMember(mVal, "symbol");
                            msig.returnType = parseNativeTypeKind(getStringMember(mVal, "returnType"));
                            msig.returnClass = getStringMember(mVal, "returnClass");
                            if (const auto* paramsArr = findMember(mVal, "paramTypes")) {
                                if (paramsArr->kind == json::Value::Kind::Array) {
                                    for (const auto& pElem : paramsArr->elements) {
                                        if (pElem && pElem->kind == json::Value::Kind::String) {
                                            msig.paramTypes.push_back(parseNativeTypeKind(toUtf8(pElem->text)));
                                        }
                                    }
                                }
                            }
                            if (!msig.symbol.empty()) {
                                clsSig.methods[mName] = msig;
                            }
                        }
                    }
                }

                // Properties
                if (const auto* propsObj = findMember(clsVal, "properties")) {
                    if (propsObj->kind == json::Value::Kind::Object) {
                        for (const auto& pMember : propsObj->members) {
                            std::string propName = toUtf8(pMember.key);
                            const auto* pVal = pMember.value.get();
                            if (!pVal) continue;

                            NativePropertySig psig;
                            psig.getterSymbol = getStringMember(pVal, "getter");
                            psig.setterSymbol = getStringMember(pVal, "setter");
                            psig.type = parseNativeTypeKind(getStringMember(pVal, "returnType"));
                            clsSig.properties[propName] = psig;
                        }
                    }
                }

                manifest.knownClasses_.insert(clsSig.name);
                manifest.classes_[clsKey] = clsSig;
                if (!clsSig.name.empty() && clsSig.name != clsKey) {
                    manifest.classes_[clsSig.name] = clsSig;
                }
            }
        }
    }

    // 3. Parse flat symbols array
    if (const auto* symbolsArr = findMember(root.get(), "symbols")) {
        if (symbolsArr->kind == json::Value::Kind::Array) {
            for (const auto& elem : symbolsArr->elements) {
                if (!elem || elem->kind != json::Value::Kind::Object) continue;
                std::string kind = getStringMember(elem.get(), "kind");
                std::string jsPath = getStringMember(elem.get(), "jsPath");
                if (jsPath.empty()) continue;

                auto dotPos = jsPath.find('.');
                std::string rootNs = (dotPos != std::string::npos) ? jsPath.substr(0, dotPos) : jsPath;
                manifest.namespaceRoots_.insert(rootNs);

                if (kind == "function") {
                    NativeFunctionSig sig;
                    sig.symbol = getStringMember(elem.get(), "symbol");
                    sig.returnType = parseNativeTypeKind(getStringMember(elem.get(), "returnType"));
                    sig.returnClass = getStringMember(elem.get(), "returnClass");

                    if (const auto* paramsArr = findMember(elem.get(), "paramTypes")) {
                        if (paramsArr->kind == json::Value::Kind::Array) {
                            for (const auto& pElem : paramsArr->elements) {
                                if (pElem && pElem->kind == json::Value::Kind::String) {
                                    sig.paramTypes.push_back(parseNativeTypeKind(toUtf8(pElem->text)));
                                }
                            }
                        }
                    }
                    if (!sig.symbol.empty()) {
                        manifest.functions_[jsPath] = sig;
                    }
                } else if (kind == "property") {
                    NativePropertySig psig;
                    psig.getterSymbol = getStringMember(elem.get(), "getter");
                    psig.setterSymbol = getStringMember(elem.get(), "setter");
                    psig.type = parseNativeTypeKind(getStringMember(elem.get(), "returnType"));
                    manifest.namespaceProperties_[jsPath] = psig;
                }
            }
        }
    }

    return manifest;
}

std::optional<NativeManifest> NativeManifest::loadFromDirectory(const std::string& dirPath, std::string& err) {
    std::error_code ec;
    if (!std::filesystem::exists(dirPath, ec) || !std::filesystem::is_directory(dirPath, ec)) {
        err = "Directory does not exist: " + dirPath;
        return std::nullopt;
    }

    NativeManifest combined;
    bool foundAny = false;
    for (const auto& entry : std::filesystem::directory_iterator(dirPath, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const auto p = entry.path();
        if (p.extension() == ".json") {
            std::string subErr;
            auto subManifest = loadFromFile(p.string(), subErr);
            if (subManifest) {
                combined.merge(*subManifest);
                foundAny = true;
            }
        }
    }

    if (!foundAny) {
        err = "No manifest JSON files found in " + dirPath;
        return std::nullopt;
    }

    return combined;
}

bool NativeManifest::merge(const NativeManifest& other) {
    for (const auto& [k, v] : other.functions_) {
        functions_[k] = v;
    }
    for (const auto& [k, v] : other.namespaceProperties_) {
        namespaceProperties_[k] = v;
    }
    for (const auto& [k, v] : other.classes_) {
        classes_[k] = v;
    }
    for (const auto& ns : other.namespaceRoots_) {
        namespaceRoots_.insert(ns);
    }
    for (const auto& cls : other.knownClasses_) {
        knownClasses_.insert(cls);
    }
    for (const auto& lib : other.extraLibPaths_) {
        extraLibPaths_.push_back(lib);
    }
    return true;
}

const NativeFunctionSig* NativeManifest::findFunction(const std::string& qualifiedName) const {
    auto it = functions_.find(qualifiedName);
    if (it != functions_.end()) return &it->second;
    return nullptr;
}

const NativePropertySig* NativeManifest::findNamespaceProperty(const std::string& qualifiedName) const {
    auto it = namespaceProperties_.find(qualifiedName);
    if (it != namespaceProperties_.end()) return &it->second;
    return nullptr;
}

const NativeClassSig* NativeManifest::findClass(const std::string& nameOrQualified) const {
    auto it = classes_.find(nameOrQualified);
    if (it != classes_.end()) return &it->second;
    return nullptr;
}

bool NativeManifest::isNamespaceRoot(const std::string& name) const {
    return namespaceRoots_.contains(name);
}

bool NativeManifest::isKnownClass(const std::string& name) const {
    return knownClasses_.contains(name);
}

}  // namespace bronze::lower
