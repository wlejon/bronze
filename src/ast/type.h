#pragma once

// TypeScript types as the parser reads them: a tree, built once and read by
// whatever wants to know what an annotation says.
//
// bronze treats an annotation as an untrusted hint. Inference decides what a
// value is, and lowering only compares the hint with the proof to say
// something when they disagree. So nothing here is a type system: a node
// records what the source wrote, and `TypeScope` records which declaration
// each written name refers to, so that the reader can follow an alias or
// recognise a class without re-reading text. The tree is immutable once
// built and shared (`TypeNodePtr`), because a cloned function body names the
// same annotation the original did.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "support/source.h"

namespace bronze::ast {

struct TypeNode;
struct TypeScope;
using TypeNodePtr = std::shared_ptr<const TypeNode>;

enum class TypeNodeKind : uint8_t {
    // A predefined name the grammar reads as a keyword: `any`, `unknown`,
    // `never`, `void`, `undefined`, `null`, `number`, `string`, `boolean`,
    // `bigint`, `symbol`, `object`, `this`, and bronze's own IL spellings
    // (`f64`, `i32`, `bool`, `str`, `dynamic`). `name` is the spelling.
    Keyword,
    // A literal type: `"a"`, `1`, `-1`, `1n`, `true`, `false`. `name` is the
    // source text and `literal` says which kind it is.
    Literal,
    // A type reference, `A`, `ns.A`, `A<B, C>`: `name` is the dotted name as
    // written, `types` the type arguments, and `scope` where it was written,
    // which is what the name is resolved against (see TypeScope).
    Reference,
    Union,         // `types` are the members
    Intersection,  // `types` are the members
    Array,         // `T[]`: `types[0]` is the element
    Tuple,         // `[A, b?: B, ...C[]]`: `elements`
    // `(a: A) => R`, `new (a: A) => R`, `abstract new () => R`: `params`,
    // `typeParams`, and the result in `types[0]`.
    Function,
    Object,        // `{ a: A; m(): R; [k: string]: V }`: `members`
    // `keyof T`, `readonly T`, `unique symbol`: `name` is the operator and
    // `types[0]` the operand.
    Operator,
    // `typeof a.b<T>`: `name` is the dotted entity, `types` its arguments.
    Query,
    Indexed,       // `T[K]`: `types[0]` is the object, `types[1]` the index
    // `C extends E ? T : F`: `types` are C, E, T and F in that order.
    Conditional,
    // `infer U` / `infer U extends C`: `name` is U, `types[0]` the constraint
    // when there is one.
    Infer,
    // `{ readonly [K in C as N]?: T }`: `typeParams[0]` is K with C as its
    // constraint, `types[0]` is N (or null) and `types[1]` the value type (or
    // null). `readonlyMod` / `optionalMod` are `+`, `-`, or ' ' when the
    // modifier is present without a sign, and 0 when it is absent.
    Mapped,
    // `` `a${T}b` ``: `quasis` are the text pieces (one more than `types`),
    // `types` the substitutions.
    TemplateLiteral,
    // `import("m").A.B<T>`: `name` is the specifier, `qualifier` the dotted
    // member path (possibly empty), `types` the arguments.
    Import,
    // A return type `x is T`, `asserts x`, `asserts x is T`: `name` is the
    // parameter (or `this`), `asserts` says which form, `types[0]` is T when
    // the form has one.
    Predicate,
    Parenthesized,  // `(T)`: `types[0]`
};

enum class TypeLiteralKind : uint8_t { String, Number, BigInt, Boolean };

// One parameter of a function type or of a method / call signature.
struct TypeFnParam {
    std::string name;  // empty for a destructuring pattern, `this` for a this-parameter
    TypeNodePtr type;  // null when the parameter has no annotation
    bool rest = false;
    bool optional = false;
};

// A generic's parameter, `T extends C = D`. The `in` / `out` / `const`
// modifiers say nothing a hint can use and are not kept.
struct TypeParameter {
    std::string name;
    TypeNodePtr constraint;   // may be null
    TypeNodePtr defaultType;  // may be null
};

struct TypeTupleElement {
    std::string label;  // `[x: A]`; empty when unlabelled
    TypeNodePtr type;
    bool rest = false;
    bool optional = false;
};

enum class TypeMemberKind : uint8_t {
    Property,   // `a: T`, `a?: T`, `readonly a: T`
    Method,     // `m(a: A): R`, `m?<T>(): R`
    Call,       // `(a: A): R`
    Construct,  // `new (a: A): R`
    Index,      // `[k: K]: V`: params[0] is the key, `type` the value
    Getter,     // `get a(): T`
    Setter,     // `set a(v: T)`
};

struct TypeMember {
    TypeMemberKind kind = TypeMemberKind::Property;
    // The key as written: an identifier, a string or number literal's source
    // text, or `[expr]` for a computed key (whose expression is not kept: it
    // is a value, and nothing about a hint depends on it).
    std::string name;
    bool optional = false;
    bool readonly = false;
    std::vector<TypeParameter> typeParams;
    std::vector<TypeFnParam> params;
    // A property's type, a signature's return, an index signature's value.
    // Null where the source wrote none.
    TypeNodePtr type;
};

struct TypeNode {
    TypeNodeKind kind = TypeNodeKind::Keyword;
    Span span;
    std::string name;
    std::vector<TypeNodePtr> types;
    // Reference and Query only: the scope the name was written in.
    const TypeScope* scope = nullptr;
    TypeLiteralKind literal = TypeLiteralKind::String;       // Literal
    std::vector<std::string> qualifier;                      // Import
    std::vector<TypeParameter> typeParams;                   // Function, Mapped
    std::vector<TypeFnParam> params;                         // Function
    std::vector<TypeTupleElement> elements;                  // Tuple
    std::vector<TypeMember> members;                         // Object
    std::vector<std::string> quasis;                         // TemplateLiteral
    bool isConstructor = false;                              // Function: `new (...) => R`
    bool isAbstract = false;                                 // Function: `abstract new`
    bool asserts = false;                                    // Predicate
    char readonlyMod = 0;                                    // Mapped
    char optionalMod = 0;                                    // Mapped
};

// What a type name can name. A name may carry several at once — TypeScript
// merges an interface into a class, a namespace into an enum — so these are
// bits.
enum TypeDeclKind : uint8_t {
    kTypeDeclAlias = 1u << 0,
    kTypeDeclInterface = 1u << 1,
    kTypeDeclClass = 1u << 2,
    kTypeDeclEnum = 1u << 3,
    kTypeDeclNamespace = 1u << 4,
    // A generic's own parameter, `infer U`, a mapped type's key: a name that
    // stands for whatever type is supplied, which is no hint at all.
    kTypeDeclParameter = 1u << 5,
    // `import * as ns`, or `export * as ns`: a module's type exports, read by
    // a qualified name (`ns.T`).
    kTypeDeclModuleNamespace = 1u << 6,
};

struct TypeDecl;
using TypeDeclPtr = std::shared_ptr<TypeDecl>;
// A module's type exports, by exported name. Shared, because a namespace
// import holds its target's table and both outlive the linker that made it.
using TypeExportTable = std::unordered_map<std::string, TypeDeclPtr>;

struct TypeDecl {
    std::string name;
    uint8_t kinds = 0;
    // An alias's right-hand side. Its references were written in the alias's
    // own scope (the one holding its type parameters), so reading it needs
    // nothing but the node.
    TypeNodePtr aliased;
    // A class's `extends` clause as a reference to the base, null when the
    // class extends nothing or something that is not a name. Whether an
    // instance is an ordinary object depends on what it extends: a class
    // extending `Array` makes arrays.
    TypeNodePtr heritage;
    bool classExtends = false;
    // kTypeDeclModuleNamespace: the module's type exports.
    std::shared_ptr<const TypeExportTable> members;
};

// One lexical scope's type declarations. The parser opens one for the module,
// every block and function body, every class, and every generic, and each
// reference it builds points at the scope it was written in. Resolution is
// LATE — a type may be used above its declaration, and a module's imports
// are bound into its root scope only by the linker — so a name is looked up
// by whoever reads the tree, once the tree is complete.
struct TypeScope {
    const TypeScope* parent = nullptr;
    std::unordered_map<std::string, TypeDeclPtr> decls;

    // The innermost declaration of `name` visible from this scope, or null.
    const TypeDecl* lookup(std::string_view name) const;
};

// `import type { a as b } from "m"`, `import { type a } from "m"`, `import
// type d from "m"` (`imported` is "default"), `import type * as ns from "m"`.
struct TypeImport {
    std::string imported;
    std::string local;
    std::string specifier;
    bool isNamespace = false;
    Span span;
};

// One type name a module exports. Without `hasFrom`, `local` names a type in
// the module's root scope; with it, `local` is the other module's export name.
// `isStar` is `export type * from "m"` (every type export of the target but
// `default`), or with `exported` set, `export type * as ns from "m"`.
struct TypeExport {
    std::string local;
    std::string exported;
    bool hasFrom = false;
    std::string fromSpecifier;
    bool isStar = false;
    Span span;
};

// The text of a type as TypeScript would print it, for diagnostics and AST
// dumps. It is a rendering, never an interface: nothing reads meaning back
// out of it.
std::string printType(const TypeNode& type);
inline std::string printType(const TypeNodePtr& type) {
    return type ? printType(*type) : std::string();
}

// Whether `type` is, or is a union containing, a reference to a type named
// `name`: `Float32Array`, `Float32Array | null`, `(Float32Array)`.
bool namesType(const TypeNode& type, std::string_view name);

}  // namespace bronze::ast
