// Type names across modules: each module's declarations are its own, and an
// importer sees only what it imports. These read the merged module's type
// trees and follow each annotation's scope, which is what lowering does to
// find the declaration a name means.

#include <filesystem>
#include <fstream>
#include <string>

#include <doctest/doctest.h>

#include "../test_temp_dir.h"
#include "ast/ast.h"
#include "ast/dump.h"
#include "modules/modules.h"

using namespace bronze;

namespace {

class TypeSandbox {
public:
    explicit TypeSandbox(const std::string& name)
        : root_(bronze_test::tempDir() / ("bronze_module_types_" + name)) {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
        std::filesystem::create_directories(root_, ec);
    }
    ~TypeSandbox() {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    std::string write(const std::string& relative, const std::string& text) const {
        std::filesystem::path path = root_ / relative;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary);
        out << text;
        return path.string();
    }

private:
    std::filesystem::path root_;
};

struct Linked {
    SourceSet sources;
    std::unique_ptr<ast::Module> module;
    std::string errors;
};

void link(Linked& out, const std::string& entry) {
    DiagnosticSink diags;
    out.module = modules::loadProgram(entry, out.sources, diags);
    out.errors = diags.render(out.sources);
}

// The declaration that the annotation on the top-level variable whose
// (possibly renamed) name ends in `name` resolves `typeName` to.
const ast::TypeDecl* resolve(const ast::Module& mod, const std::string& name,
                             const std::string& typeName) {
    for (const auto& stmt : mod.body) {
        const auto* v = dynamic_cast<const ast::VarDecl*>(stmt.get());
        if (v == nullptr || v->typeAnnotation == nullptr) continue;
        const std::string& n = v->name;
        const bool matches = n == name || (n.size() > name.size() &&
                                           n.compare(n.size() - name.size(), name.size(), name) == 0 &&
                                           n[n.size() - name.size() - 1] == '.');
        if (!matches) continue;
        // A union's members carry the scope; the union itself names nothing.
        const ast::TypeNode* named = v->typeAnnotation.get();
        while (named->scope == nullptr && !named->types.empty()) named = named->types[0].get();
        REQUIRE(named->scope != nullptr);
        return named->scope->lookup(typeName);
    }
    FAIL("no annotated variable " << name);
    return nullptr;
}

}  // namespace

TEST_CASE("two modules' private types of one name stay apart") {
    TypeSandbox box("private");
    box.write("a.ts", "type Id = number;\nexport let aId: Id = 1;\n");
    box.write("b.ts", "type Id = string;\nexport let bId: Id = 'b';\n");
    const std::string entry = box.write(
        "main.ts",
        "import { aId } from './a.ts';\nimport { bId } from './b.ts';\n"
        "let mine: Id = [];\nconsole.log(aId, bId, mine);\n");
    Linked r;
    link(r, entry);
    REQUIRE_MESSAGE(r.module != nullptr, r.errors);
    const ast::TypeDecl* a = resolve(*r.module, "aId", "Id");
    const ast::TypeDecl* b = resolve(*r.module, "bId", "Id");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a->aliased->name == "number");
    CHECK(b->aliased->name == "string");
    // Neither is visible in the importer, which has no `Id` of its own.
    CHECK(resolve(*r.module, "mine", "Id") == nullptr);
}

TEST_CASE("an importer sees the types it imports, under the names it gives them") {
    TypeSandbox box("imports");
    box.write("shapes.ts",
              "type Hidden = number;\n"
              "export type Area = number;\n"
              "export interface Shape { area(): Area }\n"
              "export class Circle { r = 1; }\n"
              "export default class Square {}\n"
              "export const unit = 1;\n");
    box.write("again.ts", "export type { Area as Size } from './shapes.ts';\nexport * from './shapes.ts';\n");
    const std::string entry = box.write(
        "main.ts",
        "import type { Shape } from './shapes.ts';\n"
        "import Square, { Circle, type Area as A, unit } from './shapes.ts';\n"
        "import * as ns from './shapes.ts';\n"
        "import { type Size, type Area } from './again.ts';\n"
        "let s: Shape | null = null;\n"
        "let c: Circle = new Circle();\n"
        "let q: Square = new Square();\n"
        "let a: A = unit;\n"
        "let n: ns.Area = 1;\n"
        "let z: Size = 2;\n"
        "let w: Area = 3;\n"
        "let h: Hidden = 4;\n"
        "console.log(s, c, q, a, n, z, w, h, ns.unit);\n");
    Linked r;
    link(r, entry);
    REQUIRE_MESSAGE(r.module != nullptr, r.errors);

    const ast::TypeDecl* shape = resolve(*r.module, "s", "Shape");
    REQUIRE(shape != nullptr);
    CHECK((shape->kinds & ast::kTypeDeclInterface) != 0);
    const ast::TypeDecl* circle = resolve(*r.module, "c", "Circle");
    REQUIRE(circle != nullptr);
    CHECK((circle->kinds & ast::kTypeDeclClass) != 0);
    const ast::TypeDecl* square = resolve(*r.module, "q", "Square");
    REQUIRE(square != nullptr);
    CHECK((square->kinds & ast::kTypeDeclClass) != 0);
    const ast::TypeDecl* area = resolve(*r.module, "a", "A");
    REQUIRE(area != nullptr);
    REQUIRE(area->aliased != nullptr);
    CHECK(area->aliased->name == "number");
    // A namespace import is a namespace of the module's exported types.
    const ast::TypeDecl* ns = resolve(*r.module, "n", "ns");
    REQUIRE(ns != nullptr);
    CHECK((ns->kinds & ast::kTypeDeclModuleNamespace) != 0);
    REQUIRE(ns->members != nullptr);
    CHECK(ns->members->count("Area") == 1);
    CHECK(ns->members->count("Hidden") == 0);
    // Re-exports, renamed and by `export *`, reach the same declaration.
    CHECK(resolve(*r.module, "z", "Size") == area);
    CHECK(resolve(*r.module, "w", "Area") == area);
    // A type the module does not export reaches no one.
    CHECK(resolve(*r.module, "h", "Hidden") == nullptr);
}

TEST_CASE("a file imported only for its types is read for them, and is no module of the program") {
    TypeSandbox box("type_only");
    box.write("base.ts", "export type Count = number;\nconsole.log('never run');\n");
    box.write("types.ts",
              "import type { Count } from './base.ts';\n"
              "export type { Count };\n"
              "export * from './base.ts';\n"
              "export interface Point { x: Count }\n");
    const std::string entry = box.write(
        "main.ts",
        "import type { Count, Point } from './types.ts';\n"
        "import type { Gone } from './missing.ts';\n"
        "let c: Count = 1;\n"
        "let p: Point | null = null;\n"
        "let g: Gone | null = null;\n"
        "console.log(c, p, g);\n");
    Linked r;
    link(r, entry);
    REQUIRE_MESSAGE(r.module != nullptr, r.errors);
    const ast::TypeDecl* count = resolve(*r.module, "c", "Count");
    REQUIRE(count != nullptr);
    REQUIRE(count->aliased != nullptr);
    CHECK(count->aliased->name == "number");
    const ast::TypeDecl* point = resolve(*r.module, "p", "Point");
    REQUIRE(point != nullptr);
    CHECK((point->kinds & ast::kTypeDeclInterface) != 0);
    // A specifier that names no file binds a type of no kind, which hints
    // nothing and is no error.
    const ast::TypeDecl* gone = resolve(*r.module, "g", "Gone");
    REQUIRE(gone != nullptr);
    CHECK(gone->kinds == 0);
    // Neither file joined the program: none of its code runs.
    CHECK(ast::dump(*r.module).find("never run") == std::string::npos);
}

TEST_CASE("an import does not displace the importer's own declaration") {
    TypeSandbox box("shadow");
    box.write("lib.ts", "export type Id = number;\nexport const one = 1;\n");
    const std::string entry = box.write(
        "main.ts",
        "import { one } from './lib.ts';\n"
        "type Id = string;\n"
        "let mine: Id = 'x';\n"
        "console.log(one, mine);\n");
    Linked r;
    link(r, entry);
    REQUIRE_MESSAGE(r.module != nullptr, r.errors);
    const ast::TypeDecl* id = resolve(*r.module, "mine", "Id");
    REQUIRE(id != nullptr);
    CHECK(id->aliased->name == "string");
}
