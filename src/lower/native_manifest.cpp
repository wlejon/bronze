#include "lower/native_manifest.h"

#include <algorithm>
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

// A string member, or a refusal: a member that is absent (when required) or
// not a string is an error here, never "".
bool stringMember(const json::Value* obj, std::string_view key, bool required, std::string& out,
                  std::string& err) {
    const auto* v = findMember(obj, key);
    if (!v) {
        if (required) {
            err = "missing \"" + std::string(key) + "\"";
            return false;
        }
        out.clear();
        return true;
    }
    if (v->kind != json::Value::Kind::String) {
        err = "\"" + std::string(key) + "\" must be a string";
        return false;
    }
    out = toUtf8(v->text);
    return true;
}

bool parseKind(std::string_view text, NativeKind& out) {
    if (text == "function") { out = NativeKind::Function; return true; }
    if (text == "method") { out = NativeKind::Method; return true; }
    if (text == "constructor") { out = NativeKind::Constructor; return true; }
    if (text == "getter") { out = NativeKind::Getter; return true; }
    if (text == "setter") { out = NativeKind::Setter; return true; }
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

std::string NativeManifest::key(NativeKind kind, std::string_view path) {
    return std::string(nativeKindName(kind)) + " " + std::string(path);
}

bool NativeManifest::add(NativeSig sig, std::string& err) {
    const std::string k = key(sig.kind, sig.path);
    if (index_.contains(k)) {
        err = "'" + k + "' declared twice";
        return false;
    }
    if (sig.kind == NativeKind::Constructor) classes_.insert(sig.path);
    const auto dot = sig.path.find('.');
    const std::string root = (dot == std::string::npos) ? sig.path : sig.path.substr(0, dot);
    if (dot != std::string::npos && std::find(roots_.begin(), roots_.end(), root) == roots_.end()) {
        roots_.push_back(root);
    }
    index_[k] = entries_.size();
    entries_.push_back(std::move(sig));
    return true;
}

std::optional<NativeManifest> NativeManifest::parse(std::string_view text, std::string_view origin,
                                                    std::string& err) {
    auto fail = [&](const std::string& what) {
        err = "native manifest " + std::string(origin) + ": " + what;
        return std::nullopt;
    };
    std::string jsonErr;
    json::ValuePtr root = json::parse(toUnits(text), jsonErr);
    if (!root) return fail("invalid JSON: " + jsonErr);
    if (root->kind != json::Value::Kind::Object) return fail("the top level must be an object");
    for (const char* old : {"namespaces", "symbols"}) {
        if (findMember(root.get(), old)) {
            return fail(std::string("\"") + old +
                        "\" is the prototype manifest form, which is no longer read; write the "
                        "manifest with embed::writeNativeManifest (a flat \"natives\" array)");
        }
    }
    if (const auto* classes = findMember(root.get(), "classes")) {
        (void)classes;
        return fail("\"classes\" is the prototype manifest form, which is no longer read; a class "
                    "is its \"constructor\" entry in the flat \"natives\" array");
    }
    const auto* version = findMember(root.get(), "version");
    if (!version || version->kind != json::Value::Kind::Number || version->number != 1.0) {
        return fail("\"version\" must be 1");
    }
    const auto* natives = findMember(root.get(), "natives");
    if (!natives || natives->kind != json::Value::Kind::Array) {
        return fail("\"natives\" must be an array");
    }

    NativeManifest manifest;
    // Two passes, so an entry may name a class whose constructor comes later
    // in the file: the registry required constructor-first at registration,
    // but a file is a snapshot and its order carries no meaning.
    for (const auto& elem : natives->elements) {
        if (!elem || elem->kind != json::Value::Kind::Object) {
            return fail("every \"natives\" entry must be an object");
        }
        std::string path, kindText, memberErr;
        if (!stringMember(elem.get(), "path", true, path, memberErr) ||
            !stringMember(elem.get(), "kind", true, kindText, memberErr)) {
            return fail("entry: " + memberErr);
        }
        if (kindText == "constructor") manifest.classes_.insert(path);
    }

    for (size_t idx = 0; idx < natives->elements.size(); ++idx) {
        const json::Value* elem = natives->elements[idx].get();
        NativeSig sig;
        std::string kindText, returns, memberErr;
        if (!stringMember(elem, "path", true, sig.path, memberErr) ||
            !stringMember(elem, "kind", true, kindText, memberErr) ||
            !stringMember(elem, "class", false, sig.className, memberErr) ||
            !stringMember(elem, "returns", true, returns, memberErr) ||
            !stringMember(elem, "returnClass", false, sig.returnClass, memberErr)) {
            return fail("entry " + std::to_string(idx) + ": " + memberErr);
        }
        auto refuse = [&](const std::string& what) {
            return fail("'" + sig.path + "' (" + kindText + "): " + what);
        };
        if (!abi::isNativeJsPath(sig.path)) return refuse("not a JS path");
        if (!parseKind(kindText, sig.kind)) {
            return refuse("unknown kind (function, method, constructor, getter, setter)");
        }

        auto resolveType = [&](const std::string& t, bool asParam, abi::NativeTypeRef& out,
                               std::string& why) {
            abi::NativeType scalar;
            if (abi::parseNativeScalarType(t, scalar)) {
                if (asParam && scalar == abi::NativeType::Void) {
                    why = "a parameter cannot be 'void'";
                    return false;
                }
                out.kind = scalar;
                out.className.clear();
                return true;
            }
            if (manifest.classes_.contains(t)) {
                out.kind = abi::NativeType::Class;
                out.className = t;
                return true;
            }
            why = "unknown type '" + t + "': not a vocabulary keyword and no constructor in this "
                  "manifest declares a class of that name";
            return false;
        };

        std::string why;
        if (sig.kind == NativeKind::Constructor) {
            if (!sig.className.empty() && sig.className != sig.path) {
                return refuse("a constructor's \"class\" is its own path");
            }
            sig.className = sig.path;
            if (!returns.empty() && returns != sig.path) {
                return refuse("a constructor returns its own class");
            }
            sig.returnType.kind = abi::NativeType::Class;
            sig.returnType.className = sig.path;
        } else {
            if (manifest.classes_.contains(sig.path)) return refuse("the path is a class");
            if (!resolveType(returns, false, sig.returnType, why)) return refuse("\"returns\": " + why);
            if (sig.kind == NativeKind::Method && sig.className.empty()) {
                return refuse("a method needs a \"class\"");
            }
            if (!sig.className.empty()) {
                if (sig.kind == NativeKind::Function) {
                    return refuse("\"class\" is only for method, getter, setter and constructor");
                }
                if (!manifest.classes_.contains(sig.className)) {
                    return refuse("\"class\" '" + sig.className +
                                  "' has no constructor entry in this manifest");
                }
                const std::string prefix = sig.className + ".";
                if (sig.path.size() <= prefix.size() ||
                    sig.path.compare(0, prefix.size(), prefix) != 0 ||
                    sig.path.find('.', prefix.size()) != std::string::npos) {
                    return refuse("a member's path must be '" + sig.className + ".<member>'");
                }
            } else if ((sig.kind == NativeKind::Getter || sig.kind == NativeKind::Setter) &&
                       sig.path.find('.') == std::string::npos) {
                return refuse("a namespace property needs a dotted path");
            }
        }

        const auto* params = findMember(elem, "params");
        if (!params || params->kind != json::Value::Kind::Array) {
            return refuse("\"params\" must be an array of type strings");
        }
        for (const auto& p : params->elements) {
            if (!p || p->kind != json::Value::Kind::String) {
                return refuse("\"params\" must be an array of type strings");
            }
            abi::NativeTypeRef ref;
            if (!resolveType(toUtf8(p->text), true, ref, why)) return refuse("\"params\": " + why);
            sig.paramTypes.push_back(std::move(ref));
        }
        if (!sig.returnClass.empty()) {
            if (sig.returnType.kind != abi::NativeType::Dynamic) {
                return refuse("\"returnClass\" is only for \"returns\": \"dynamic\"");
            }
            if (!manifest.classes_.contains(sig.returnClass)) {
                return refuse("\"returnClass\" '" + sig.returnClass + "' is not a class here");
            }
        }
        if (sig.kind == NativeKind::Getter &&
            (!sig.paramTypes.empty() || sig.returnType.kind == abi::NativeType::Void)) {
            return refuse("a getter takes no parameters and returns a value");
        }
        if (sig.kind == NativeKind::Setter &&
            (sig.paramTypes.size() != 1 || sig.returnType.kind != abi::NativeType::Void)) {
            return refuse("a setter takes exactly one parameter and returns void");
        }

        sig.signature = abi::nativeSignatureText(sig.returnType, sig.hasSelf(), sig.paramTypes.data(),
                                                 sig.paramTypes.size());
        std::string declared;
        if (!stringMember(elem, "signature", false, declared, memberErr)) {
            return refuse(memberErr);
        }
        if (!declared.empty() && declared != sig.signature) {
            return refuse("\"signature\" says " + declared + " but the types say " + sig.signature);
        }
        std::string addErr;
        if (!manifest.add(std::move(sig), addErr)) return fail(addErr);
    }
    return manifest;
}

std::optional<NativeManifest> NativeManifest::loadFromFile(const std::string& path, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "cannot open native manifest file: " + path;
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse(ss.str(), path, err);
}

std::optional<NativeManifest> NativeManifest::loadFromDirectory(const std::string& dirPath,
                                                                std::string& err) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dirPath, ec)) {
        err = "native manifest directory does not exist: " + dirPath;
        return std::nullopt;
    }
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dirPath, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    if (files.empty()) {
        err = "no *.json native manifests in " + dirPath;
        return std::nullopt;
    }
    // Sorted, so the merged manifest — and the import table order it drives —
    // does not depend on a directory listing's order.
    std::sort(files.begin(), files.end());
    NativeManifest combined;
    for (const auto& file : files) {
        auto part = loadFromFile(file.string(), err);
        if (!part) return std::nullopt;
        for (const auto& sig : part->entries()) {
            std::string addErr;
            if (!combined.add(sig, addErr)) {
                err = "native manifest " + file.string() + ": " + addErr + " (across the directory)";
                return std::nullopt;
            }
        }
        // A class declared in one file and referenced in another resolves
        // because each file was checked alone, and add() re-registers classes.
    }
    return combined;
}

const NativeSig* NativeManifest::findFunction(std::string_view path) const {
    auto it = index_.find(key(NativeKind::Function, path));
    return it == index_.end() ? nullptr : &entries_[it->second];
}

const NativeSig* NativeManifest::findConstructor(std::string_view classPath) const {
    auto it = index_.find(key(NativeKind::Constructor, classPath));
    return it == index_.end() ? nullptr : &entries_[it->second];
}

const NativeSig* NativeManifest::findMethod(std::string_view classPath, std::string_view member) const {
    auto it = index_.find(key(NativeKind::Method, std::string(classPath) + "." + std::string(member)));
    if (it == index_.end()) return nullptr;
    const NativeSig& sig = entries_[it->second];
    return sig.className == classPath ? &sig : nullptr;
}

const NativeSig* NativeManifest::findGetter(std::string_view owner, std::string_view member) const {
    auto it = index_.find(key(NativeKind::Getter, std::string(owner) + "." + std::string(member)));
    return it == index_.end() ? nullptr : &entries_[it->second];
}

const NativeSig* NativeManifest::findSetter(std::string_view owner, std::string_view member) const {
    auto it = index_.find(key(NativeKind::Setter, std::string(owner) + "." + std::string(member)));
    return it == index_.end() ? nullptr : &entries_[it->second];
}

}  // namespace bronze::lower
