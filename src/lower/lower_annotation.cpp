// TypeScript type annotation evaluation and checking against proven types.

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "lower/lowerer.h"

namespace bronze::lower {
namespace {

constexpr const char* kReadableAnnotations =
    "any, bool, boolean, dynamic, f64, i32, never, null, number, object, str, string, "
    "undefined, unknown, void";

std::string_view trimView(std::string_view sv) {
    while (!sv.empty() &&
           (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\n' || sv.front() == '\r')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() &&
           (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\n' || sv.back() == '\r')) {
        sv.remove_suffix(1);
    }
    return sv;
}

bool hasMatchingOuterParens(std::string_view sv) {
    if (sv.size() < 2 || sv.front() != '(' || sv.back() != ')') return false;
    int depth = 0;
    for (size_t i = 0; i < sv.size(); ++i) {
        const char c = sv[i];
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            --depth;
            if (depth == 0 && i + 1 < sv.size()) return false;
        }
    }
    return depth == 0;
}

// Split `sv` by top-level `sep` ('|' or '&'), ignoring occurrences inside
// `(...)`, `[...]`, `{...}`, `<...>`, or string literals, and ignoring `=>`.
std::vector<std::string_view> splitTopLevel(std::string_view sv, char sep) {
    std::vector<std::string_view> parts;
    int paren = 0, bracket = 0, brace = 0, angle = 0;
    char quote = 0;
    size_t start = 0;
    for (size_t i = 0; i < sv.size(); ++i) {
        const char c = sv[i];
        if (quote != 0) {
            if (c == '\\' && i + 1 < sv.size()) {
                ++i;
            } else if (c == quote) {
                quote = 0;
            }
            continue;
        }
        if (c == '\'' || c == '"' || c == '`') {
            quote = c;
            continue;
        }
        if (c == '(') ++paren;
        else if (c == ')' && paren > 0) --paren;
        else if (c == '[') ++bracket;
        else if (c == ']' && bracket > 0) --bracket;
        else if (c == '{') ++brace;
        else if (c == '}' && brace > 0) --brace;
        else if (c == '<') ++angle;
        else if (c == '>' && angle > 0 && !(i > 0 && sv[i - 1] == '=')) --angle;
        else if (c == sep && paren == 0 && bracket == 0 && brace == 0 && angle == 0) {
            parts.push_back(trimView(sv.substr(start, i - start)));
            start = i + 1;
        }
    }
    if (!parts.empty()) {
        parts.push_back(trimView(sv.substr(start)));
    }
    return parts;
}

bool isTopLevelArrowOrConditional(std::string_view sv, bool& isArrowOut) {
    int paren = 0, bracket = 0, brace = 0, angle = 0;
    char quote = 0;
    for (size_t i = 0; i < sv.size(); ++i) {
        const char c = sv[i];
        if (quote != 0) {
            if (c == '\\' && i + 1 < sv.size()) ++i;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '\'' || c == '"' || c == '`') {
            quote = c;
            continue;
        }
        if (c == '(') ++paren;
        else if (c == ')' && paren > 0) --paren;
        else if (c == '[') ++bracket;
        else if (c == ']' && bracket > 0) --bracket;
        else if (c == '{') ++brace;
        else if (c == '}' && brace > 0) --brace;
        else if (c == '<') ++angle;
        else if (c == '>' && angle > 0 && !(i > 0 && sv[i - 1] == '=')) --angle;
        else if (paren == 0 && bracket == 0 && brace == 0 && angle == 0) {
            if (c == '=' && i + 1 < sv.size() && sv[i + 1] == '>') {
                isArrowOut = true;
                return true;
            }
            if (c == '?') {
                isArrowOut = false;
                return true;
            }
        }
    }
    return false;
}

bool isBuiltinTypeName(std::string_view base) {
    static const std::unordered_set<std::string_view> kBuiltins = {
        "Array", "ReadonlyArray", "ConcatArray", "ArrayLike",
        "Promise", "PromiseLike", "Awaited",
        "Map", "ReadonlyMap", "Set", "ReadonlySet", "WeakMap", "WeakSet", "WeakRef",
        "Record", "Partial", "Required", "Readonly", "Pick", "Omit",
        "Exclude", "Extract", "NonNullable", "Parameters", "ConstructorParameters",
        "ReturnType", "InstanceType", "ThisParameterType", "OmitThisParameter",
        "ThisType", "Uppercase", "Lowercase", "Capitalize", "Uncapitalize", "NoInfer",
        "Iterable", "Iterator", "IterableIterator", "AsyncIterable", "AsyncIterator",
        "AsyncIterableIterator", "Generator", "AsyncGenerator", "IteratorResult",
        "PropertyKey", "PropertyDescriptor", "PropertyDescriptorMap",
        "Date", "RegExp", "RegExpMatchArray", "RegExpExecArray",
        "Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError",
        "URIError", "EvalError", "AggregateError",
        "ArrayBuffer", "SharedArrayBuffer", "ArrayBufferLike", "ArrayBufferView", "DataView",
        "Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array", "Uint16Array",
        "Int32Array", "Uint32Array", "Float32Array", "Float64Array",
        "BigInt64Array", "BigUint64Array",
        "FinalizationRegistry", "Proxy", "Reflect", "JSON", "Math", "Intl",
        "import",
    };
    return kBuiltins.count(base) != 0;
}

std::optional<types::Type> evaluateAnnotation(std::string_view raw, const ast::Module& mod,
                                              int depth = 0) {
    if (depth > 16) return types::Type::dynamic();
    std::string_view sv = trimView(raw);
    if (sv.empty()) return std::nullopt;

    while (hasMatchingOuterParens(sv)) {
        sv = trimView(sv.substr(1, sv.size() - 2));
    }

    // Prefix type operators: `readonly T`, `keyof T`, `typeof x`, `infer U`, `unique symbol`
    if (sv.rfind("readonly ", 0) == 0) {
        return evaluateAnnotation(sv.substr(9), mod, depth + 1);
    }
    if (sv.rfind("keyof ", 0) == 0 || sv.rfind("typeof ", 0) == 0 || sv.rfind("infer ", 0) == 0 ||
        sv.rfind("unique ", 0) == 0) {
        return types::Type::dynamic();
    }

    // Top-level conditional type `T extends U ? X : Y` or function type `(...) => R`
    bool isArrow = false;
    if (isTopLevelArrowOrConditional(sv, isArrow)) {
        return isArrow ? types::Type::function() : types::Type::dynamic();
    }

    // Top-level union `A | B`
    auto unionParts = splitTopLevel(sv, '|');
    if (unionParts.size() > 1) {
        std::optional<types::Type> joined;
        for (std::string_view part : unionParts) {
            if (part.empty()) continue;
            auto t = evaluateAnnotation(part, mod, depth + 1);
            if (!t) return std::nullopt;
            joined = joined ? types::join(*joined, *t) : *t;
        }
        return joined ? joined : types::Type::dynamic();
    }

    // Top-level intersection `A & B`
    auto interParts = splitTopLevel(sv, '&');
    if (interParts.size() > 1) {
        for (std::string_view part : interParts) {
            if (part.empty()) continue;
            if (!evaluateAnnotation(part, mod, depth + 1)) return std::nullopt;
        }
        return types::Type::dynamic();
    }

    // Object type literal `{ ... }`
    if (sv.front() == '{' && sv.back() == '}') {
        return types::Type::object();
    }

    // Tuple type `[A, B]`
    if (sv.front() == '[' && sv.back() == ']') {
        return types::Type::array();
    }

    // Postfix array `T[]` or indexed access `T[K]`
    if (sv.size() > 2 && sv.back() == ']') {
        int bracket = 0;
        size_t openPos = std::string_view::npos;
        for (size_t i = sv.size(); i-- > 0;) {
            if (sv[i] == ']') ++bracket;
            else if (sv[i] == '[') {
                if (--bracket == 0) {
                    openPos = i;
                    break;
                }
            }
        }
        if (openPos != std::string_view::npos && openPos > 0) {
            std::string_view prefix = trimView(sv.substr(0, openPos));
            std::string_view inside = trimView(sv.substr(openPos + 1, sv.size() - openPos - 2));
            if (!evaluateAnnotation(prefix, mod, depth + 1)) return std::nullopt;
            return inside.empty() ? types::Type::array() : types::Type::dynamic();
        }
    }

    // String literal type `'...'`, `"..."`, `` `...` ``
    if (sv.size() >= 2 &&
        ((sv.front() == '\'' && sv.back() == '\'') ||
         (sv.front() == '"' && sv.back() == '"') ||
         (sv.front() == '`' && sv.back() == '`'))) {
        return types::Type::string();
    }

    // Numeric or BigInt literal type
    if (!sv.empty()) {
        std::string_view numView = sv;
        if (numView.front() == '-' || numView.front() == '+') numView.remove_prefix(1);
        if (!numView.empty() && numView.front() >= '0' && numView.front() <= '9') {
            if (numView.back() == 'n') return types::Type::dynamic();
            return types::Type::number();
        }
    }

    // Core primitive & special types
    if (sv == "number" || sv == "Number" || sv == "f64" || sv == "i32") {
        return types::Type::number();
    }
    if (sv == "boolean" || sv == "Boolean" || sv == "bool" || sv == "true" || sv == "false") {
        return types::Type::boolean();
    }
    if (sv == "string" || sv == "String" || sv == "str") {
        return types::Type::string();
    }
    if (sv == "bigint" || sv == "BigInt" || sv == "symbol" || sv == "Symbol") {
        return types::Type::dynamic();
    }
    if (sv == "undefined" || sv == "void") {
        return types::Type::undefined();
    }
    if (sv == "null") {
        return types::Type::null();
    }
    if (sv == "object" || sv == "Object") {
        return types::Type::object();
    }
    if (sv == "Function") {
        return types::Type::function();
    }
    if (sv == "never") {
        return types::Type::never();
    }
    if (sv == "any" || sv == "unknown" || sv == "dynamic" || sv == "this") {
        return types::Type::dynamic();
    }

    // Extract base name before `<...>` if generic arguments are present
    std::string_view base = sv;
    const size_t lt = sv.find('<');
    if (lt != std::string_view::npos && sv.back() == '>') {
        base = trimView(sv.substr(0, lt));
    }

    if (base == "Array" || base == "ReadonlyArray" || base == "ConcatArray") {
        return types::Type::array();
    }

    // Check module type aliases first
    const std::string baseStr(base);
    auto aliasIt = mod.typeAliases.find(baseStr);
    if (aliasIt != mod.typeAliases.end() && lt == std::string_view::npos) {
        auto resolved = evaluateAnnotation(aliasIt->second, mod, depth + 1);
        if (resolved) return resolved;
        return types::Type::dynamic();
    }

    // Check module declared types (interfaces, classes, enums, type params, imported types)
    if (mod.declaredTypes.count(baseStr) != 0) {
        return types::Type::dynamic();
    }

    // Check qualified type names `ns.Member`: if the root `ns` is a declared type or `import`
    const size_t dot = base.find('.');
    if (dot != std::string_view::npos) {
        const std::string rootName(trimView(base.substr(0, dot)));
        if (rootName == "import" || rootName == "Intl" || rootName == "NodeJS" ||
            mod.declaredTypes.count(rootName) != 0) {
            return types::Type::dynamic();
        }
    }

    if (isBuiltinTypeName(base)) {
        return types::Type::dynamic();
    }

    return std::nullopt;
}

}  // namespace

// A TS annotation is an untrusted optimization hint.
//
// Note what it does NOT do: it does not return a type. The caller has already
// chosen its IL type from what inference proved; this only decides whether to
// say something about the annotation.
bool Lowerer::checkAnnotation(const std::string& ann, Span span, const std::string& name,
                              types::Type proven) {
    if (ann.empty()) return true;
    const auto claim = evaluateAnnotation(ann, astModule_);
    if (!claim) {
        diags_.error(span, "unsupported type annotation: " + ann +
                               " (bronze reads: " + kReadableAnnotations + ")");
        return false;
    }
    if (claim->is(types::TypeKind::Dynamic)) return true;
    if (claim->kind() == proven.kind()) return true;
    if (inference_ == nullptr) return true;

    const std::string observed = types::typeKindName(proven.kind());
    if (proven.is(types::TypeKind::Dynamic) || proven.is(types::TypeKind::Never)) {
        diags_.warning(span, "annotation '" + ann + "' on '" + name +
                                 "' is not provable; ignoring (inferred: " + observed + ")");
    } else {
        diags_.warning(span, "annotation '" + ann + "' on '" + name +
                                 "' contradicts inferred " + observed);
    }
    return true;
}

}  // namespace bronze::lower
