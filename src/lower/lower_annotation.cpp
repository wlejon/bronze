// TypeScript type annotations: the hint each one gives, read off the type
// tree the parser built, and the check of that hint against what inference
// proved.

#include <string>
#include <string_view>

#include "ast/type.h"
#include "lower/lower.h"
#include "lower/lowerer.h"

namespace bronze::lower {
namespace {

// Past this many aliases deep the hint is given up on rather than followed:
// `type A = B; type B = A` is legal TypeScript, and a hint is never worth a
// cycle.
constexpr int kMaxAliasDepth = 16;

types::Type hintOf(const ast::TypeNode& type, int depth);

// A name no declaration in scope answers. The ECMAScript constructors whose
// instances are one of the lattice's kinds hint that kind, as do bronze's own
// IL spellings; every other name — a DOM type, a library's, one from a
// `.d.ts` this compilation never read — hints nothing, which is what an
// annotation bronze cannot read must do: the program is still a program.
types::Type unresolvedHint(std::string_view name) {
    if (name == "Number" || name == "f64" || name == "i32") return types::Type::number();
    if (name == "Boolean" || name == "bool") return types::Type::boolean();
    if (name == "String" || name == "str") return types::Type::string();
    if (name == "Object") return types::Type::object();
    if (name == "Function") return types::Type::function();
    if (name == "Array" || name == "ReadonlyArray" || name == "ConcatArray") {
        return types::Type::array();
    }
    return types::Type::dynamic();
}

// Whether instances of a class extending the unresolved global `name` are
// ordinary objects: everything but the constructors whose instances are
// arrays, functions or typed arrays, which are kinds of their own.
bool unresolvedBaseMakesObjects(std::string_view name) {
    if (name == "Array" || name == "Function") return false;
    return name.size() < 5 || name.substr(name.size() - 5) != "Array";
}

types::Type hintOfDecl(const ast::TypeDecl& decl, int depth) {
    if (decl.kinds & ast::kTypeDeclParameter) return types::Type::dynamic();
    if (decl.kinds & ast::kTypeDeclAlias) {
        return decl.aliased ? hintOf(*decl.aliased, depth + 1) : types::Type::dynamic();
    }
    if (decl.kinds & ast::kTypeDeclClass) {
        // An instance of a class is an object, unless what it extends makes
        // it something else: `class L extends Array` makes arrays.
        if (!decl.classExtends) return types::Type::object();
        const ast::TypeNode* base = decl.heritage.get();
        if (base == nullptr || depth > kMaxAliasDepth) return types::Type::dynamic();
        const ast::TypeDecl* baseDecl =
            base->scope != nullptr ? base->scope->lookup(base->name) : nullptr;
        if (baseDecl == nullptr) {
            return unresolvedBaseMakesObjects(base->name) ? types::Type::object()
                                                          : types::Type::dynamic();
        }
        const types::Type baseHint = hintOfDecl(*baseDecl, depth + 1);
        return baseHint.is(types::TypeKind::Object) ? baseHint : types::Type::dynamic();
    }
    // An interface may describe a function or an array as well as an object,
    // and an enum's members are numbers or strings: none names one kind.
    return types::Type::dynamic();
}

// A reference names a declaration in the scope it was written in, the
// module's imports included (the linker bound them into its root scope).
// `ns.T` reads `T` from a module namespace `ns`; any other qualified name is
// a namespace's member, which hints nothing.
types::Type hintOfReference(const ast::TypeNode& ref, int depth) {
    const std::string_view name = ref.name;
    const size_t dot = name.find('.');
    const std::string_view root = name.substr(0, dot);
    const ast::TypeDecl* decl = ref.scope != nullptr ? ref.scope->lookup(root) : nullptr;
    if (dot == std::string_view::npos) {
        return decl != nullptr ? hintOfDecl(*decl, depth) : unresolvedHint(name);
    }
    const std::string_view rest = name.substr(dot + 1);
    if (decl == nullptr || (decl->kinds & ast::kTypeDeclModuleNamespace) == 0 ||
        decl->members == nullptr || rest.find('.') != std::string_view::npos) {
        return types::Type::dynamic();
    }
    auto it = decl->members->find(std::string(rest));
    return it != decl->members->end() && it->second ? hintOfDecl(*it->second, depth)
                                                    : types::Type::dynamic();
}

types::Type hintOfKeyword(std::string_view name) {
    if (name == "number") return types::Type::number();
    if (name == "string") return types::Type::string();
    if (name == "boolean") return types::Type::boolean();
    if (name == "undefined" || name == "void") return types::Type::undefined();
    if (name == "null") return types::Type::null();
    if (name == "object") return types::Type::object();
    if (name == "never") return types::Type::never();
    // `any`, `unknown`, `this`, `bigint`, `symbol`: the lattice has no kind
    // for the last two, and the first three claim nothing.
    return types::Type::dynamic();
}

types::Type hintOf(const ast::TypeNode& type, int depth) {
    if (depth > kMaxAliasDepth) return types::Type::dynamic();
    switch (type.kind) {
        case ast::TypeNodeKind::Keyword: return hintOfKeyword(type.name);
        case ast::TypeNodeKind::Literal:
            switch (type.literal) {
                case ast::TypeLiteralKind::String: return types::Type::string();
                case ast::TypeLiteralKind::Number: return types::Type::number();
                case ast::TypeLiteralKind::Boolean: return types::Type::boolean();
                case ast::TypeLiteralKind::BigInt: return types::Type::dynamic();
            }
            return types::Type::dynamic();
        case ast::TypeNodeKind::Reference: return hintOfReference(type, depth);
        case ast::TypeNodeKind::Union: {
            types::Type joined = types::Type::never();
            bool first = true;
            for (const auto& member : type.types) {
                const types::Type t = member ? hintOf(*member, depth + 1) : types::Type::dynamic();
                joined = first ? t : types::join(joined, t);
                first = false;
            }
            return first ? types::Type::dynamic() : joined;
        }
        case ast::TypeNodeKind::Array:
        case ast::TypeNodeKind::Tuple: return types::Type::array();
        case ast::TypeNodeKind::Function: return types::Type::function();
        case ast::TypeNodeKind::Object: {
            // `{}` is every value but null and undefined, numbers included.
            if (type.members.empty()) return types::Type::dynamic();
            // A type with a call or construct signature describes a function.
            for (const auto& member : type.members) {
                if (member.kind == ast::TypeMemberKind::Call ||
                    member.kind == ast::TypeMemberKind::Construct) {
                    return types::Type::function();
                }
            }
            return types::Type::object();
        }
        case ast::TypeNodeKind::Operator:
            // `readonly T[]` is still an array; `keyof T` is a set of keys,
            // and `unique symbol` a symbol.
            if (type.name == "readonly" && !type.types.empty() && type.types[0]) {
                return hintOf(*type.types[0], depth + 1);
            }
            return types::Type::dynamic();
        case ast::TypeNodeKind::TemplateLiteral: return types::Type::string();
        case ast::TypeNodeKind::Predicate:
            return type.asserts ? types::Type::undefined() : types::Type::boolean();
        case ast::TypeNodeKind::Parenthesized:
            return type.types[0] ? hintOf(*type.types[0], depth + 1) : types::Type::dynamic();
        // What these denote depends on type-level computation bronze does not
        // do (an index into a type, a conditional, a mapping over keys) or on
        // a value's declared type (`typeof x`), so they hint nothing.
        case ast::TypeNodeKind::Intersection:
        case ast::TypeNodeKind::Query:
        case ast::TypeNodeKind::Indexed:
        case ast::TypeNodeKind::Conditional:
        case ast::TypeNodeKind::Infer:
        case ast::TypeNodeKind::Mapped:
        case ast::TypeNodeKind::Import: return types::Type::dynamic();
    }
    return types::Type::dynamic();
}

}  // namespace

types::Type annotationHint(const ast::TypeNode& type) { return hintOf(type, 0); }

// A TS annotation is an untrusted optimization hint.
//
// Note what it does NOT do: it does not return a type. The caller has already
// chosen its IL type from what inference proved; this only decides whether to
// say something about the annotation.
void Lowerer::checkAnnotation(const ast::TypeNodePtr& ann, Span span, const std::string& name,
                              types::Type proven) {
    if (!ann) return;
    const types::Type claim = annotationHint(*ann);
    if (claim.is(types::TypeKind::Dynamic)) return;
    if (claim.kind() == proven.kind()) return;
    if (inference_ == nullptr) return;

    const std::string text = ast::printType(*ann);
    const std::string observed = types::typeKindName(proven.kind());
    if (proven.is(types::TypeKind::Dynamic) || proven.is(types::TypeKind::Never)) {
        diags_.warning(span, "annotation '" + text + "' on '" + name +
                                 "' is not provable; ignoring (inferred: " + observed + ")");
    } else {
        diags_.warning(span, "annotation '" + text + "' on '" + name +
                                 "' contradicts inferred " + observed);
    }
}

}  // namespace bronze::lower
