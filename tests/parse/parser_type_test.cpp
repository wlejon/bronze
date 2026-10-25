// The type tree (ast/type.h): what the parser builds for each form of
// TypeScript type, and the scopes its names are declared and referenced in.

#include <doctest/doctest.h>

#include <memory>
#include <string>

#include "ast/ast.h"
#include "lex/lexer.h"
#include "parse/parser.h"

using namespace bronze;
using ast::TypeNodeKind;

namespace {

struct Parsed {
    std::unique_ptr<ast::Module> mod;
    std::string errors;
};

Parsed parseTs(std::string_view src) {
    SourceBuffer buf("t.ts", std::string(src));
    DiagnosticSink diags;
    auto tokens = Lexer(buf, diags).lex();
    REQUIRE_FALSE(diags.hasErrors());
    Parsed out;
    out.mod = Parser(std::move(tokens), diags).setTypeScript(true).parseModule("t.ts");
    if (diags.hasErrors()) out.errors = diags.render(buf);
    return out;
}

// The annotation of the first `let`/`const` in `src`.
ast::TypeNodePtr typeOf(std::string_view src) {
    Parsed p = parseTs(src);
    INFO(p.errors);
    REQUIRE(p.mod != nullptr);
    for (const auto& stmt : p.mod->body) {
        if (const auto* v = dynamic_cast<const ast::VarDecl*>(stmt.get())) {
            REQUIRE(v->typeAnnotation.get() != nullptr);
            return v->typeAnnotation;
        }
    }
    FAIL("no declarator");
    return nullptr;
}

ast::TypeNodePtr typeOfAnnotation(const std::string& type) {
    return typeOf("let v: " + type + ";\n");
}

}  // namespace

TEST_CASE("primitives, literals and references are their own nodes") {
    auto t = typeOfAnnotation("number");
    CHECK(t->kind == TypeNodeKind::Keyword);
    CHECK(t->name == "number");
    CHECK(typeOfAnnotation("void")->kind == TypeNodeKind::Keyword);
    CHECK(typeOfAnnotation("null")->kind == TypeNodeKind::Keyword);

    auto s = typeOfAnnotation("'on'");
    CHECK(s->kind == TypeNodeKind::Literal);
    CHECK(s->literal == ast::TypeLiteralKind::String);
    CHECK(typeOfAnnotation("-1")->literal == ast::TypeLiteralKind::Number);
    CHECK(typeOfAnnotation("-1")->name == "-1");
    CHECK(typeOfAnnotation("10n")->literal == ast::TypeLiteralKind::BigInt);
    CHECK(typeOfAnnotation("true")->literal == ast::TypeLiteralKind::Boolean);

    auto r = typeOfAnnotation("Map<string, ns.Item[]>");
    REQUIRE(r->kind == TypeNodeKind::Reference);
    CHECK(r->name == "Map");
    REQUIRE(r->types.size() == 2);
    CHECK(r->types[0]->kind == TypeNodeKind::Keyword);
    REQUIRE(r->types[1]->kind == TypeNodeKind::Array);
    CHECK(r->types[1]->types[0]->kind == TypeNodeKind::Reference);
    CHECK(r->types[1]->types[0]->name == "ns.Item");
    CHECK(r->scope != nullptr);
    // `>>` closing two argument lists is split into two `>`.
    CHECK(ast::printType(typeOfAnnotation("A<B<C>>")) == "A<B<C>>");
}

TEST_CASE("unions, intersections and their precedence") {
    auto t = typeOfAnnotation("| A | B & C | D[]");
    REQUIRE(t->kind == TypeNodeKind::Union);
    REQUIRE(t->types.size() == 3);
    CHECK(t->types[0]->kind == TypeNodeKind::Reference);
    CHECK(t->types[1]->kind == TypeNodeKind::Intersection);
    CHECK(t->types[1]->types.size() == 2);
    CHECK(t->types[2]->kind == TypeNodeKind::Array);
    // A leading `|` with one member is that member.
    CHECK(typeOfAnnotation("| A")->kind == TypeNodeKind::Reference);
    // `keyof T[]` is `keyof (T[])`, and parentheses are kept.
    auto k = typeOfAnnotation("keyof T[]");
    REQUIRE(k->kind == TypeNodeKind::Operator);
    CHECK(k->name == "keyof");
    CHECK(k->types[0]->kind == TypeNodeKind::Array);
    auto p = typeOfAnnotation("(A | B)[]");
    REQUIRE(p->kind == TypeNodeKind::Array);
    CHECK(p->types[0]->kind == TypeNodeKind::Parenthesized);
    CHECK(typeOfAnnotation("readonly (A | B)[]")->kind == TypeNodeKind::Operator);
}

TEST_CASE("function and constructor types keep their parameters") {
    auto f = typeOfAnnotation("<T>(a: T, b?: string, ...c: boolean[]) => void");
    REQUIRE(f->kind == TypeNodeKind::Function);
    REQUIRE(f->typeParams.size() == 1);
    CHECK(f->typeParams[0].name == "T");
    REQUIRE(f->params.size() == 3);
    CHECK(f->params[0].name == "a");
    CHECK(f->params[1].optional);
    CHECK(f->params[2].rest);
    CHECK(f->types[0]->name == "void");

    auto c = typeOfAnnotation("abstract new (x: number) => Foo");
    REQUIRE(c->kind == TypeNodeKind::Function);
    CHECK(c->isConstructor);
    CHECK(c->isAbstract);

    // A parenthesized type is not a parameter list without the `=>`.
    CHECK(typeOfAnnotation("(string)")->kind == TypeNodeKind::Parenthesized);
    // A destructuring parameter binds no name a type can see.
    auto d = typeOfAnnotation("({ a }: { a: number }, [b]: [string]) => void");
    REQUIRE(d->params.size() == 2);
    CHECK(d->params[0].name.empty());
    CHECK(d->params[0].type->kind == TypeNodeKind::Object);
    // `x is T` and `asserts x` are return types.
    auto g = typeOfAnnotation("(x: unknown) => x is string");
    REQUIRE(g->types[0]->kind == TypeNodeKind::Predicate);
    CHECK_FALSE(g->types[0]->asserts);
    CHECK(typeOfAnnotation("(x: unknown) => asserts x")->types[0]->asserts);
}

TEST_CASE("tuples, object types and their members") {
    auto t = typeOfAnnotation("[x: number, y?: string, ...rest: T[]]");
    REQUIRE(t->kind == TypeNodeKind::Tuple);
    REQUIRE(t->elements.size() == 3);
    CHECK(t->elements[0].label == "x");
    CHECK(t->elements[1].optional);
    CHECK(t->elements[2].rest);
    CHECK(typeOfAnnotation("[A, B?]")->elements[1].optional);

    auto o = typeOfAnnotation(
        "{ a: number; readonly b?: string, m<T>(x: T): T\n"
        "  (): void; new (): X; [k: string]: unknown; get g(): number; set g(v: number);\n"
        "  [Symbol.iterator](): Iterator<number>; 'q': 1; readonly: boolean }");
    REQUIRE(o->kind == TypeNodeKind::Object);
    REQUIRE(o->members.size() == 11);
    CHECK(o->members[0].kind == ast::TypeMemberKind::Property);
    CHECK(o->members[1].readonly);
    CHECK(o->members[1].optional);
    CHECK(o->members[2].kind == ast::TypeMemberKind::Method);
    CHECK(o->members[2].typeParams.size() == 1);
    CHECK(o->members[3].kind == ast::TypeMemberKind::Call);
    CHECK(o->members[4].kind == ast::TypeMemberKind::Construct);
    CHECK(o->members[5].kind == ast::TypeMemberKind::Index);
    CHECK(o->members[6].kind == ast::TypeMemberKind::Getter);
    CHECK(o->members[7].kind == ast::TypeMemberKind::Setter);
    CHECK(o->members[8].name == "[Symbol.iterator]");
    CHECK(o->members[9].name == "'q'");
    CHECK(o->members[10].name == "readonly");
    CHECK_FALSE(o->members[10].readonly);
}

TEST_CASE("the type operators bronze reads but does not compute") {
    auto q = typeOfAnnotation("typeof a.b<number>");
    REQUIRE(q->kind == TypeNodeKind::Query);
    CHECK(q->name == "a.b");
    CHECK(q->types.size() == 1);

    auto i = typeOfAnnotation("T['k']");
    REQUIRE(i->kind == TypeNodeKind::Indexed);
    CHECK(i->types[1]->kind == TypeNodeKind::Literal);

    auto c = typeOfAnnotation("T extends Array<infer U extends string> ? U : never");
    REQUIRE(c->kind == TypeNodeKind::Conditional);
    REQUIRE(c->types.size() == 4);
    const auto& arg = c->types[1]->types[0];
    REQUIRE(arg->kind == TypeNodeKind::Infer);
    CHECK(arg->name == "U");
    CHECK(arg->types.size() == 1);
    // In a conditional's extends type `infer U extends X` is a constraint
    // even before a `?`; elsewhere the `?` makes the `extends` a conditional.
    auto c2 = typeOfAnnotation("T extends infer U extends string ? U : never");
    REQUIRE(c2->kind == TypeNodeKind::Conditional);
    REQUIRE(c2->types[1]->kind == TypeNodeKind::Infer);
    CHECK(c2->types[1]->types.size() == 1);
    auto c3 = typeOfAnnotation("T extends [infer U extends string ? 1 : 2] ? U : never");
    REQUIRE(c3->kind == TypeNodeKind::Conditional);
    REQUIRE(c3->types[1]->kind == TypeNodeKind::Tuple);
    const auto& inner = c3->types[1]->elements[0].type;
    REQUIRE(inner->kind == TypeNodeKind::Conditional);
    CHECK(inner->types[0]->kind == TypeNodeKind::Infer);
    CHECK(inner->types[0]->types.empty());

    auto m = typeOfAnnotation("{ -readonly [K in keyof T as `x${K & string}`]+?: T[K] }");
    REQUIRE(m->kind == TypeNodeKind::Mapped);
    CHECK(m->readonlyMod == '-');
    CHECK(m->optionalMod == '+');
    REQUIRE(m->typeParams.size() == 1);
    CHECK(m->typeParams[0].name == "K");
    CHECK(m->types[0]->kind == TypeNodeKind::TemplateLiteral);
    CHECK(m->types[1]->kind == TypeNodeKind::Indexed);

    auto tpl = typeOfAnnotation("`a${number}b${string}c`");
    REQUIRE(tpl->kind == TypeNodeKind::TemplateLiteral);
    CHECK(tpl->quasis == std::vector<std::string>{"a", "b", "c"});
    CHECK(tpl->types.size() == 2);

    auto imp = typeOfAnnotation("import('./m').A.B<C>");
    REQUIRE(imp->kind == TypeNodeKind::Import);
    CHECK(imp->name == "./m");
    CHECK(imp->qualifier == std::vector<std::string>{"A", "B"});
    CHECK(imp->types.size() == 1);

    CHECK(typeOfAnnotation("unique symbol")->name == "unique");
}

TEST_CASE("a type prints as TypeScript writes it") {
    for (const char* text : {
             "number | undefined", "Map<string, number[]>", "(a: number, b?: string) => void",
             "new (x: T) => Foo", "[x: number, y?: string]", "{ a: number; m(): void }",
             "keyof typeof obj", "T extends U ? X : Y", "{ readonly [K in keyof T]?: T[K] }",
             "`a${number}`", "import(\"m\").A", "(A | B)[]", "readonly string[]", "{}"}) {
        CHECK(ast::printType(typeOfAnnotation(text)) == text);
    }
}

TEST_CASE("type declarations go into the scope they are written in") {
    Parsed p = parseTs(
        "type Id = number;\n"
        "interface Shape { area(): number }\n"
        "class Point {}\n"
        "enum Kind { A }\n"
        "function f<T>(x: T): T { type Local = string; let y: Local = ''; return x; }\n"
        "let id: Id = 1;\n");
    INFO(p.errors);
    REQUIRE(p.mod != nullptr);
    const ast::TypeScope* root = p.mod->rootTypeScope();
    const ast::TypeDecl* id = root->lookup("Id");
    REQUIRE(id != nullptr);
    CHECK(id->kinds == ast::kTypeDeclAlias);
    REQUIRE(id->aliased.get() != nullptr);
    CHECK(id->aliased->name == "number");
    CHECK(root->lookup("Shape")->kinds == ast::kTypeDeclInterface);
    CHECK(root->lookup("Point")->kinds == ast::kTypeDeclClass);
    CHECK((root->lookup("Kind")->kinds & ast::kTypeDeclEnum) != 0);
    // A function's type parameters and its body's types are its own.
    CHECK(root->lookup("T") == nullptr);
    CHECK(root->lookup("Local") == nullptr);

    // Type declarations leave no statement; an enum and a class do.
    const ast::FunctionDecl* fn = nullptr;
    for (const auto& stmt : p.mod->body) {
        if (const auto* f = dynamic_cast<const ast::FunctionDecl*>(stmt.get())) fn = f;
    }
    REQUIRE(fn != nullptr);
    const ast::TypeNode& param = *fn->params[0].typeAnnotation;
    REQUIRE(param.scope != nullptr);
    REQUIRE(param.scope->lookup("T") != nullptr);
    CHECK(param.scope->lookup("T")->kinds == ast::kTypeDeclParameter);
    // A reference finds the module's declaration through its scope chain.
    const auto* decl = dynamic_cast<const ast::VarDecl*>(p.mod->body.back().get());
    REQUIRE(decl != nullptr);
    CHECK(decl->typeAnnotation->scope->lookup("Id") == id);
}

TEST_CASE("an optional parameter's annotation includes undefined") {
    Parsed p = parseTs("function f(a?: number) { return a; }\n");
    REQUIRE(p.mod != nullptr);
    const auto* fn = dynamic_cast<const ast::FunctionDecl*>(p.mod->body[0].get());
    REQUIRE(fn != nullptr);
    const auto& ann = fn->params[0].typeAnnotation;
    REQUIRE(ann->kind == TypeNodeKind::Union);
    CHECK(ast::printType(ann) == "number | undefined");
}

TEST_CASE("type-only imports and exports are recorded for the linker") {
    Parsed p = parseTs(
        "import type { A, B as C } from './a';\n"
        "import type D from './d';\n"
        "import type * as NS from './ns';\n"
        "import { type E, value } from './e';\n"
        "export type { A };\n"
        "export type { F as G } from './f';\n"
        "export type * from './star';\n"
        "export type H = number;\n"
        "export interface I {}\n"
        "export { type C };\n"
        "console.log(value);\n");
    INFO(p.errors);
    REQUIRE(p.mod != nullptr);
    const auto& imports = p.mod->typeImports;
    REQUIRE(imports.size() == 5);
    CHECK(imports[1].imported == "B");
    CHECK(imports[1].local == "C");
    CHECK(imports[2].imported == "default");
    CHECK(imports[3].isNamespace);
    CHECK(imports[4].local == "E");
    CHECK(imports[4].specifier == "./e");
    const auto& exports = p.mod->typeExports;
    REQUIRE(exports.size() == 6);
    CHECK(exports[1].hasFrom);
    CHECK(exports[1].exported == "G");
    CHECK(exports[2].isStar);
    CHECK(exports[3].local == "H");
    CHECK(exports[4].local == "I");
    CHECK(exports[5].local == "C");
}

TEST_CASE("invalid type syntax is still a syntax error") {
    CHECK_FALSE(parseTs("let a: number[;\n").errors.empty());
    CHECK_FALSE(parseTs("let b: { x: number y: string };\n").errors.empty());
    CHECK_FALSE(parseTs("let c: (a: number => void;\n").errors.empty());
    CHECK_FALSE(parseTs("let d: Map<string;\n").errors.empty());
}
