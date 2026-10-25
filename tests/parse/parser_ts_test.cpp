#include <doctest/doctest.h>

#include "ast/dump.h"
#include "lex/lexer.h"
#include "parse/parser.h"

using namespace bronze;

namespace {

std::string parseTs(std::string_view src) {
    SourceBuffer buf("t.ts", std::string(src));
    DiagnosticSink diags;
    auto tokens = Lexer(buf, diags).lex();
    REQUIRE_FALSE(diags.hasErrors());
    auto mod = Parser(std::move(tokens), diags).setTypeScript(true).parseModule("t");
    if (diags.hasErrors()) return "ERRORS:\n" + diags.render(buf);
    REQUIRE(mod != nullptr);
    return ast::dump(*mod);
}

// The same source as a `.js` file, which must read every token sequence the
// way JavaScript does.
std::string parseJs(std::string_view src) {
    SourceBuffer buf("t.js", std::string(src));
    DiagnosticSink diags;
    auto tokens = Lexer(buf, diags).lex();
    REQUIRE_FALSE(diags.hasErrors());
    auto mod = Parser(std::move(tokens), diags).parseModule("t.js");
    if (diags.hasErrors()) return "ERRORS:\n" + diags.render(buf);
    REQUIRE(mod != nullptr);
    return ast::dump(*mod);
}

}  // namespace

TEST_CASE("the TypeScript grammar is chosen by the file's extension") {
    CHECK(Parser::isTypeScriptPath("a/b.ts"));
    CHECK(Parser::isTypeScriptPath("b.mts"));
    CHECK(Parser::isTypeScriptPath("b.cts"));
    CHECK_FALSE(Parser::isTypeScriptPath("b.js"));
    CHECK_FALSE(Parser::isTypeScriptPath("b.mjs"));
    CHECK_FALSE(Parser::isTypeScriptPath("ts"));

    // `a < b > (c)` is a generic call in TypeScript and two comparisons in
    // JavaScript.
    const auto ts = parseTs("f(a < b > (c));\n");
    const auto js = parseJs("f(a < b > (c));\n");
    CHECK(ts.find("(binary <") == std::string::npos);
    CHECK(js.find("(binary >\n        (binary <") != std::string::npos);

    // TypeScript-only syntax is a syntax error in a .js file, and the
    // contextual words are identifiers there.
    CHECK(parseJs("const x = y as number;\n").find("ERRORS:") == 0);
    CHECK(parseJs("const x = y!;\n").find("ERRORS:") == 0);
    CHECK(parseJs("const x = <T>y;\n").find("ERRORS:") == 0);
    CHECK(parseJs("function f(x);\n").find("ERRORS:") == 0);
    CHECK(parseJs("let type = 1, namespace = 2; type = namespace;\n").find("ERRORS:") ==
          std::string::npos);
    // The annotation syntax bronze read in .js before it read TypeScript.
    CHECK(parseJs("let n: number = 1; function g(a: number): number { return a; }\n")
              .find("ERRORS:") == std::string::npos);
}

TEST_CASE("type alias and interface declarations are erased from the AST") {
    const auto out = parseTs(
        "type ID = string | number;\n"
        "export type Pair<A, B = A> = readonly [A, B?];\n"
        "type Lookup<T, K extends keyof T> = T[K] extends infer U ? U : never;\n"
        "type Mapped<T> = { readonly [P in keyof T as `get_${string & P}`]-?: T[P] };\n"
        "interface Point<T = number> extends Base<T>, Serializable {\n"
        "  readonly x: T;\n"
        "  y?: T;\n"
        "  [key: string]: unknown;\n"
        "  distance(other: Point<T>): number;\n"
        "  new (x: T, y: T): Point<T>;\n"
        "  ( scale: number ): Point<T>;\n"
        "}\n"
        "export interface ExportedIface { a: number }\n"
        "const alive = 42;\n");
    CHECK(out ==
          "(module t\n"
          "  (const alive\n"
          "    (number 42)\n"
          "  )\n"
          ")\n");
}

TEST_CASE("import type and inline type specifiers are erased") {
    const auto out = parseTs(
        "import type { Foo, Bar as Baz } from './types.js';\n"
        "import type DefaultType from './types.js';\n"
        "import type * as TypesNS from './types.js';\n"
        "import { type OnlyType, realValue, type Another as Alias } from './mod.js';\n"
        "import { type OnlyA, type OnlyB } from './pure_types.js';\n"
        "export type { Foo } from './types.js';\n"
        "export { type OnlyType, realValue };\n"
        "console.log(realValue);\n");
    CHECK(out ==
          "(module t\n"
          "  (import \"./mod.js\"\n"
          "    (named realValue as realValue)\n"
          "  )\n"
          "  (export\n"
          "    (name realValue as realValue)\n"
          "  )\n"
          "  (expr\n"
          "    (call\n"
          "      (member .log\n"
          "        (ident console)\n"
          "      )\n"
          "      (ident realValue)\n"
          "    )\n"
          "  )\n"
          ")\n");
}

TEST_CASE("ambient declare statements are erased") {
    const auto out = parseTs(
        "declare var AMBIENT_VAR: number;\n"
        "declare let ambientLet: string;\n"
        "declare const AMBIENT_CONST = 10;\n"
        "declare function hostFn(x: number): void;\n"
        "declare class HostClass { constructor(x: number); method(): string; }\n"
        "declare enum HostEnum { A, B }\n"
        "declare namespace HostNS { export function f(): void; }\n"
        "declare module 'virtual-mod' { export const x: number; }\n"
        "declare global { interface Window { customProp: string; } }\n"
        "export declare function exportedAmbient(): void;\n"
        "const x = 1;\n");
    CHECK(out ==
          "(module t\n"
          "  (const x\n"
          "    (number 1)\n"
          "  )\n"
          ")\n");
}

TEST_CASE("function overloads and signature-only declarations are erased") {
    const auto out = parseTs(
        "function make(x: number): number;\n"
        "function make(x: string): string;\n"
        "function make(x: number | string): number | string {\n"
        "  return x;\n"
        "}\n"
        "export function exportedOverload(a: number): number;\n"
        "export function exportedOverload(a: any): any {\n"
        "  return a;\n"
        "}\n");
    CHECK(out.find("ERRORS:") == std::string::npos);
    CHECK(out.find("(function make (x: number | string): number | string") != std::string::npos);
    CHECK(out.find("(function exportedOverload (a: any): any exported") != std::string::npos);
}

TEST_CASE("this parameter is erased from functions and methods") {
    const auto out = parseTs(
        "function bound(this: { count: number }, delta: number, label?: string): number {\n"
        "  return this.count + delta;\n"
        "}\n"
        "class Counter {\n"
        "  count: number = 0;\n"
        "  inc(this: this, step: number = 1): number {\n"
        "    return this.count += step;\n"
        "  }\n"
        "}\n");
    CHECK(out.find("ERRORS:") == std::string::npos);
    CHECK(out.find("(function bound (delta: number label: string | undefined): number") !=
          std::string::npos);
    CHECK(out.find("(function-expr Counter.inc (step: number =): number") != std::string::npos);
}

TEST_CASE("class modifiers, implements, abstract members, and parameter properties") {
    const auto out = parseTs(
        "abstract class Base<T> implements Disposable {\n"
        "  public readonly id!: number;\n"
        "  protected label?: string;\n"
        "  declare virtualField: T;\n"
        "  abstract compute(x: T): number;\n"
        "  constructor(public x: number, private readonly y: string = 'init') {\n"
        "    this.id = 1;\n"
        "  }\n"
        "  override toString(): string;\n"
        "  override toString(): string {\n"
        "    return this.y;\n"
        "  }\n"
        "}\n");
    CHECK(out.find("ERRORS:") == std::string::npos);
    // `declare virtualField: T` and uninitialized `id!: number` / `label?: string`
    // and `abstract compute` and overload `toString` are erased; constructor
    // parameter properties `public x` and `private readonly y` synthesize
    // `this.x = x` and `this.y = y` at the top of the constructor body, which
    // has no super() call to follow.
    CHECK(out.find("virtualField") == std::string::npos);
    CHECK(out.find("compute") == std::string::npos);
    CHECK(out.find("(binary =\n            (member .x\n              (this)\n            )\n            (ident x)\n          )") != std::string::npos);
    CHECK(out.find("(binary =\n            (member .y\n              (this)\n            )\n            (ident y)\n          )") != std::string::npos);
}

TEST_CASE("type assertions (as, satisfies), non-null !, and prefix <T> assertion") {
    const auto out = parseTs(
        "const a = (1 as number) + (2 satisfies number);\n"
        "const b = <string>foo!.bar!(baz! as const);\n");
    CHECK(out ==
          "(module t\n"
          "  (const a\n"
          "    (binary +\n"
          "      (number 1)\n"
          "      (number 2)\n"
          "    )\n"
          "  )\n"
          "  (const b\n"
          "    (call\n"
          "      (member .bar\n"
          "        (ident foo)\n"
          "      )\n"
          "      (ident baz)\n"
          "    )\n"
          "  )\n"
          ")\n");
}

TEST_CASE("generic arrow functions, generic calls, and relational comparison disambiguation") {
    const auto out = parseTs(
        "const id = <T extends unknown = number>(x: T): T => x;\n"
        "const y = id<number>(42);\n"
        "const cmp = a < b && c > (d + 1);\n"
        "const inst = new Map<string, Array<number>>();\n");
    CHECK(out.find("ERRORS:") == std::string::npos);
    CHECK(out.find("(arrow-expr <anon> (x: T): T") != std::string::npos);
    CHECK(out.find("(call\n      (ident id)\n      (number 42)\n    )") != std::string::npos);
    CHECK(out.find("(binary &&\n      (binary <\n        (ident a)\n        (ident b)\n      )") !=
          std::string::npos);
    CHECK(out.find("(new Map\n    )") != std::string::npos);
}

TEST_CASE("enum and const enum lower to IIFE object bindings") {
    const auto out = parseTs(
        "enum Direction { Up, Down = 10, Left, Name = 'LEFT' }\n"
        "export const enum Flags { None = 0, Read = 1 << 0, Write = 1 << 1, RW = Read | Write }\n");
    CHECK(out.find("ERRORS:") == std::string::npos);
    CHECK(out.find("(const Direction") != std::string::npos);
    CHECK(out.find("(const Flags") != std::string::npos);
    CHECK(out.find("(number 10)") != std::string::npos);
    CHECK(out.find("(number 11)") != std::string::npos);
    CHECK(out.find("(number 3)") != std::string::npos);
}

TEST_CASE("namespace lowers runtime members and erases type-only namespaces") {
    const auto out = parseTs(
        "namespace PureTypes {\n"
        "  export type T = number;\n"
        "  export interface I { x: T }\n"
        "}\n"
        "namespace MathUtil {\n"
        "  export const PI = 3;\n"
        "  export function double(x: number): number { return x * 2; }\n"
        "}\n");
    CHECK(out.find("ERRORS:") == std::string::npos);
    CHECK(out.find("PureTypes") == std::string::npos);
    // TypeScript's emit: `var MathUtil; (function (MathUtil) { MathUtil.PI =
    // 3; function double… ; MathUtil.double = double; })(MathUtil || …)`.
    CHECK(out.find("(var MathUtil") != std::string::npos);
    CHECK(out.find(".PI") != std::string::npos);
    CHECK(out.find(".double") != std::string::npos);
}

TEST_CASE("a namespace's exported variables are read through its object") {
    const auto out = parseTs(
        "namespace C {\n"
        "  export let count = 0;\n"
        "  export function inc() { count++; }\n"
        "  export function shadow(count: number) { return count; }\n"
        "}\n");
    CHECK(out.find("ERRORS:") == std::string::npos);
    // `count++` became `C.count++`; the parameter `count` stays a name.
    CHECK(out.find("(member .count\n") != std::string::npos);
    CHECK(out.find("(let count") == std::string::npos);
    CHECK(out.find("(ident count)") != std::string::npos);
}

TEST_CASE("parameter properties follow the super() call wherever it is") {
    const auto out = parseTs(
        "class D extends B {\n"
        "  constructor(public n: number) {\n"
        "    const t = n + 1;\n"
        "    if (t) { super(t); } else super(0);\n"
        "  }\n"
        "}\n");
    CHECK(out.find("ERRORS:") == std::string::npos);
    // Once after each super call and never above them.
    const size_t first = out.find("(member .n\n");
    REQUIRE(first != std::string::npos);
    CHECK(out.find("(member .n\n", first + 1) != std::string::npos);
    CHECK(out.find("(super-call") < first);

    const auto arrow = parseTs(
        "class E extends B {\n"
        "  constructor(public n: number) { const f = () => super(); f(); }\n"
        "}\n");
    CHECK(arrow.find("super() call inside an arrow function") != std::string::npos);
}

TEST_CASE("typed destructuring, catch clause, and for-of bindings") {
    const auto out = parseTs(
        "const { a, b }: { a: number; b: string } = obj;\n"
        "const [x, y]: [number, number] = pair;\n"
        "for (const item: number of list) {\n"
        "  try {\n"
        "    work(item);\n"
        "  } catch (err: unknown) {\n"
        "    handle(err);\n"
        "  }\n"
        "}\n");
    CHECK(out.find("ERRORS:") == std::string::npos);
}
