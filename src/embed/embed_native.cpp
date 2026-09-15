// The host-facing spelling of the native registry (runtime/native_registry.h):
// registration in embed.h's vocabulary, the registry printed as the manifest
// an ahead-of-time build compiles against, and the bind step a loader runs
// over a module's import table.

#include <cstdio>
#include <string>
#include <vector>

#include "embed/embed.h"
#include "runtime/fatal.h"
#include "runtime/native_handle.h"
#include "runtime/native_registry.h"

namespace bronze::embed {

namespace {

runtime::NativeKind toRuntimeKind(NativeKind kind) {
    switch (kind) {
        case NativeKind::Function: return runtime::NativeKind::Function;
        case NativeKind::Method: return runtime::NativeKind::Method;
        case NativeKind::Constructor: return runtime::NativeKind::Constructor;
        case NativeKind::Getter: return runtime::NativeKind::Getter;
        case NativeKind::Setter: return runtime::NativeKind::Setter;
    }
    return runtime::NativeKind::Function;
}

NativeKind fromRuntimeKind(runtime::NativeKind kind) {
    switch (kind) {
        case runtime::NativeKind::Function: return NativeKind::Function;
        case runtime::NativeKind::Method: return NativeKind::Method;
        case runtime::NativeKind::Constructor: return NativeKind::Constructor;
        case runtime::NativeKind::Getter: return NativeKind::Getter;
        case runtime::NativeKind::Setter: return NativeKind::Setter;
    }
    return NativeKind::Function;
}

// JSON string escaping over UTF-8 bytes: the two-character escapes, \u for
// the rest of the control range, everything else verbatim (a path is ASCII
// identifiers, but the writer does not rely on it).
std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
    return out;
}

}  // namespace

bool registerNative(std::string_view jsPath, void* fn, const NativeSignature& sig,
                    std::string* error) {
    runtime::NativeSpec spec;
    spec.kind = toRuntimeKind(sig.kind);
    spec.returnType = sig.returnType;
    spec.paramTypes = sig.paramTypes;
    spec.className = sig.className;
    spec.returnClass = sig.returnClass;
    spec.destructor = sig.destructor;
    spec.finalize = sig.finalize;
    std::string err;
    if (runtime::rtRegisterNative(jsPath, fn, spec, err)) return true;
    if (error) {
        *error = err;
        return false;
    }
    fatal(("embed: " + err).c_str());
}

bool unregisterNative(std::string_view jsPath, NativeKind kind) {
    return runtime::rtUnregisterNative(jsPath, toRuntimeKind(kind));
}

std::vector<NativeEntry> hostNatives() {
    std::vector<NativeEntry> out;
    for (const auto& e : runtime::rtNativeEntries()) {
        NativeEntry entry;
        entry.path = e.path;
        entry.fn = e.fn;
        entry.signatureText = e.signature;
        entry.signature.kind = fromRuntimeKind(e.kind);
        entry.signature.returnType = abi::nativeTypeName(e.returnType);
        for (const auto& p : e.paramTypes) {
            entry.signature.paramTypes.push_back(abi::nativeTypeName(p));
        }
        entry.signature.className = e.className;
        entry.signature.returnClass = e.returnClass;
        if (e.classInfo) {
            entry.signature.destructor = e.classInfo->destructor;
            entry.signature.finalize = e.classInfo->finalize;
        }
        out.push_back(std::move(entry));
    }
    return out;
}

std::vector<std::string> hostNativeNames() {
    std::vector<std::string> out;
    for (const auto& e : runtime::rtNativeEntries()) out.push_back(e.path);
    return out;
}

GlobalValue nativeClassPrototype(std::string_view className) {
    if (runtime::NativeClassInfo* cls = runtime::rtFindNativeClass(className)) {
        return {cls->prototype, true};
    }
    return {Value::fromUndefined(), false};
}

Value wrapNative(void* data, std::string_view className) {
    if (!data) return Value::fromNull();
    if (runtime::NativeClassInfo* cls = runtime::rtFindNativeClass(className)) {
        return runtime::rtMakeHandle(data, cls->destructor, cls->finalize, cls->prototype, cls);
    }
    return Value::fromNull();
}

// The manifest: one flat array, one entry per registration, every field the
// lowerer needs and nothing the runtime alone knows (a destructor is a
// pointer; the compiler only needs to know the class OWNS one, and it does
// not even need that). `signature` is redundant with `returns`/`params` and
// carried anyway: the reader recomputes and compares, so a hand edit that
// changed one and not the other is refused rather than trusted.
std::string nativeManifestJson() {
    std::string out = "{\n  \"version\": 1,\n  \"natives\": [";
    bool first = true;
    for (const auto& e : runtime::rtNativeEntries()) {
        out += first ? "\n" : ",\n";
        first = false;
        out += "    { \"path\": " + jsonString(e.path);
        out += ", \"kind\": " + jsonString(runtime::nativeKindName(e.kind));
        if (!e.className.empty()) out += ", \"class\": " + jsonString(e.className);
        out += ", \"returns\": " + jsonString(abi::nativeTypeName(e.returnType));
        if (!e.returnClass.empty()) out += ", \"returnClass\": " + jsonString(e.returnClass);
        out += ", \"params\": [";
        for (size_t i = 0; i < e.paramTypes.size(); ++i) {
            if (i) out += ", ";
            out += jsonString(abi::nativeTypeName(e.paramTypes[i]));
        }
        out += "], \"signature\": " + jsonString(e.signature) + " }";
    }
    out += first ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

bool writeNativeManifest(const std::string& path, std::string* error) {
    const std::string text = nativeManifestJson();
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        const std::string err = "cannot open '" + path + "' for writing";
        if (error) {
            *error = err;
            return false;
        }
        fatal(("embed: writeNativeManifest: " + err).c_str());
    }
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    if (!ok) {
        const std::string err = "short write to '" + path + "'";
        if (error) {
            *error = err;
            return false;
        }
        fatal(("embed: writeNativeManifest: " + err).c_str());
    }
    return true;
}

bool bindNativeImports(const void* importTable, std::vector<std::string>* missing) {
    return runtime::rtBindNativeImports(importTable, missing);
}

}  // namespace bronze::embed
