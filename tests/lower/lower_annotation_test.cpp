// What a TypeScript annotation hints, read off the type tree: the kinds the
// lattice can state come through (primitives, arrays, functions, a class's
// instances, an alias followed to what it names), and everything else —
// a DOM type, a library's, syntax bronze reads but does not compute with —
// hints nothing and fails nothing. The hint is only ever compared with a
// proof, so what these tests observe is the warning a contradicted hint gives
// and the hint itself (`annotationHint`), never an IL type.

#include <doctest/doctest.h>

#include <string>

#include "lower_fixture.h"

using namespace bronze;
using bronze::lower_test::inferAndLower;
using bronze::lower_test::parseAndLower;
using bronze::lower_test::parseOnly;

namespace {

// The annotation on `name`'s declarator, searched at the top level and one
// function body deep (where a block-scoped type can shadow a module's).
const ast::TypeNode* annotationOf(const ast::Module& mod, const std::string& name) {
    auto search = [&](const std::vector<ast::StmtPtr>& stmts) -> const ast::TypeNode* {
        for (const auto& stmt : stmts) {
            if (const auto* v = dynamic_cast<const ast::VarDecl*>(stmt.get())) {
                if (v->name == name) return v->typeAnnotation.get();
            }
        }
        return nullptr;
    };
    if (const ast::TypeNode* top = search(mod.body)) return top;
    for (const auto& stmt : mod.body) {
        if (const auto* fn = dynamic_cast<const ast::FunctionDecl*>(stmt.get())) {
            if (const ast::TypeNode* inner = search(fn->body)) return inner;
            for (const auto& param : fn->params) {
                if (param.name == name) return param.typeAnnotation.get();
            }
        }
    }
    return nullptr;
}

types::TypeKind hintKind(const ast::Module& mod, const std::string& name) {
    const ast::TypeNode* type = annotationOf(mod, name);
    REQUIRE(type != nullptr);
    return lower::annotationHint(*type).kind();
}

}  // namespace

TEST_CASE("hints are taken from the types bronze knows") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto mod = parseOnly(
        "type Num = number;\n"
        "type Box<T> = T[];\n"
        "type Again = Num;\n"
        "class Point { x = 1; }\n"
        "class Sub extends Point {}\n"
        "class List extends Array<number> {}\n"
        "class Failure extends Error {}\n"
        "interface Shape { area(): number }\n"
        "let a: Num = 1;\n"
        "let b: Box<string> = [];\n"
        "let c: Again = 2;\n"
        "let d: Point = new Point();\n"
        "let e: Sub = new Sub();\n"
        "let f: List = new List();\n"
        "let g: Failure = new Failure();\n"
        "let h: readonly string[] = [];\n"
        "let i: [number, string] = [1, 'a'];\n"
        "let j: (x: number) => void = () => {};\n"
        "let k: { (x: number): void } = () => {};\n"
        "let l: { a: number } = { a: 1 };\n"
        "let m: {} = 1;\n"
        "let n: Shape = { area() { return 1; } };\n"
        "let o: 'lit' = 'lit';\n"
        "let p: `a${number}` = 'a1';\n"
        "let q: (Num) = 3;\n"
        "let r: Num | string = 1;\n"
        "let s: number | number = 1;\n",
        diags, buf);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(mod != nullptr);

    CHECK(hintKind(*mod, "a") == types::TypeKind::Number);
    // A generic alias is followed whatever its arguments: `T[]` is an array
    // for every T.
    CHECK(hintKind(*mod, "b") == types::TypeKind::Array);
    CHECK(hintKind(*mod, "c") == types::TypeKind::Number);
    // A class's instances are objects, through a chain of user classes and
    // over a global base that makes objects; a class extending Array makes
    // arrays, which is no object hint.
    CHECK(hintKind(*mod, "d") == types::TypeKind::Object);
    CHECK(hintKind(*mod, "e") == types::TypeKind::Object);
    CHECK(hintKind(*mod, "f") == types::TypeKind::Dynamic);
    CHECK(hintKind(*mod, "g") == types::TypeKind::Object);
    CHECK(hintKind(*mod, "h") == types::TypeKind::Array);
    CHECK(hintKind(*mod, "i") == types::TypeKind::Array);
    CHECK(hintKind(*mod, "j") == types::TypeKind::Function);
    CHECK(hintKind(*mod, "k") == types::TypeKind::Function);
    CHECK(hintKind(*mod, "l") == types::TypeKind::Object);
    // `{}` admits numbers; an interface may describe a function or an array.
    CHECK(hintKind(*mod, "m") == types::TypeKind::Dynamic);
    CHECK(hintKind(*mod, "n") == types::TypeKind::Dynamic);
    CHECK(hintKind(*mod, "o") == types::TypeKind::String);
    CHECK(hintKind(*mod, "p") == types::TypeKind::String);
    CHECK(hintKind(*mod, "q") == types::TypeKind::Number);
    CHECK(hintKind(*mod, "r") == types::TypeKind::Dynamic);
    CHECK(hintKind(*mod, "s") == types::TypeKind::Number);
}

TEST_CASE("a type name resolves in the scope it is written in") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto mod = parseOnly(
        "type T = number;\n"
        "let outer: T = 1;\n"
        "function local() { type T = string; let inner: T = 'x'; return inner; }\n"
        "function generic<T>(param: T) { return param; }\n"
        "let later: Later = 1;\n"
        "type Later = number;\n",
        diags, buf);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(mod != nullptr);
    CHECK(hintKind(*mod, "outer") == types::TypeKind::Number);
    // The function's own alias shadows the module's inside it.
    CHECK(hintKind(*mod, "inner") == types::TypeKind::String);
    // A type parameter stands for whatever type is supplied: no hint, though
    // the module has an alias of the same name.
    CHECK(hintKind(*mod, "param") == types::TypeKind::Dynamic);
    // A type may be used above its declaration.
    CHECK(hintKind(*mod, "later") == types::TypeKind::Number);
}

TEST_CASE("a contradicted hint from an alias or a class is a warning naming the annotation") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower(
        "type Num = number;\n"
        "class Point { x = 1; }\n"
        "let a: Num = 'nope';\n"
        "let b: Point = 5;\n"
        "let c: string[] = 5;\n"
        "let d: Num = 1;\n"
        "console.log(a, b, c, d);\n",
        diags, buf);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string rendered = diags.render(buf);
    CHECK(rendered.find("warning: annotation 'Num' on 'a' contradicts inferred string") !=
          std::string::npos);
    CHECK(rendered.find("warning: annotation 'Point' on 'b' contradicts inferred number") !=
          std::string::npos);
    CHECK(rendered.find("warning: annotation 'string[]' on 'c' contradicts inferred number") !=
          std::string::npos);
    CHECK(rendered.find("on 'd'") == std::string::npos);
}

// The types a bro app names everywhere, and names from declaration files
// bronze never reads. Each hints nothing, so none is checked against a proof,
// and neither mode reports anything about them.
static constexpr const char* kUnknownTypes =
    "let el: HTMLElement | null = null;\n"
    "function onKey(e: KeyboardEvent, w: Window): void { console.log(e, w); }\n"
    "function f(x: Widget) { return x; }\n"
    "let p: Promise<Foo> | undefined = undefined;\n"
    "let q: lib.Thing<number>[] = [];\n"
    "declare const host: HostApi;\n"
    "console.log(el, f(1), p, q.length);\n";

TEST_CASE("an annotation naming a type bronze does not know hints nothing and fails nothing") {
    for (const bool infer : {true, false}) {
        DiagnosticSink diags;
        SourceBuffer buf("test.ts", "");
        const auto optMod = infer ? inferAndLower(kUnknownTypes, diags, buf)
                                  : parseAndLower(kUnknownTypes, diags, buf);
        REQUIRE_FALSE(diags.hasErrors());
        REQUIRE(optMod.has_value());
        const std::string rendered = diags.render(buf);
        CHECK(rendered.find("annotation") == std::string::npos);
    }
}

TEST_CASE("every valid shape of type compiles, and the ones bronze does not compute hint nothing") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower(
        "type Keys<T> = keyof T;\n"
        "type Mapped<T> = { readonly [K in keyof T as `get${string & K}`]-?: () => T[K] };\n"
        "type Elem<T> = T extends readonly (infer U extends string)[] ? U : never;\n"
        "type Ctor = abstract new (...args: any[]) => object;\n"
        "type Guard = (x: unknown) => x is string;\n"
        "type Assert = (x: unknown) => asserts x is number;\n"
        "type Sym = { readonly key: unique symbol };\n"
        "type Imported = import('./other').Thing<number>;\n"
        "type Query = typeof console.log;\n"
        "type Labelled = [first: number, second?: string, ...rest: boolean[]];\n"
        "type Members = {\n"
        "  (x: number): string\n"
        "  new <T>(x: T): Members\n"
        "  get size(): number\n"
        "  set size(v: number)\n"
        "  method?<T = number>(this: Members, [a, b]: [T, T], { c }: { c: T }): void;\n"
        "  [key: string]: unknown, [Symbol.iterator](): Iterator<number>\n"
        "  'quoted': 1; 42: 2; readonly: boolean; get: number;\n"
        "};\n"
        "class Base<T> { value?: T; }\n"
        "class Derived extends Base<string[]> implements Iterable<number> {\n"
        "  *[Symbol.iterator]() { yield 1; }\n"
        "}\n"
        "let k: Keys<{ a: 1 }> = 'a';\n"
        "let m: Mapped<{ a: number }> | null = null;\n"
        "let e: Elem<string[]> = 'x';\n"
        "let q: Query = console.log;\n"
        "let i: Imported | undefined = undefined;\n"
        "let t: Labelled = [1];\n"
        "let d: Derived = new Derived();\n"
        "console.log(k, m, e, typeof q, i, t.length, d.value);\n",
        diags, buf);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string rendered = diags.render(buf);
    CHECK(rendered.find("on 'k'") == std::string::npos);
    CHECK(rendered.find("on 'm'") == std::string::npos);
    CHECK(rendered.find("on 'e'") == std::string::npos);
    CHECK(rendered.find("on 'q'") == std::string::npos);
    CHECK(rendered.find("on 'i'") == std::string::npos);
}
