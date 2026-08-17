// Before doctest: its in-TU implementation includes windows.h, whose
// IMAGE_FILE_MACHINE_* macros shred BinaryFormat/COFF.h's enum of the same
// names if that header is seen second.
#include <llvm/BinaryFormat/COFF.h>
#include <llvm/Object/COFF.h>
#include <llvm/Object/ObjectFile.h>

// Captured here for the same reason: windows.h below redefines the name as a
// macro, so spelling it at the use site no longer parses.
constexpr uint32_t kCoffComdatFlag = llvm::COFF::IMAGE_SCN_LNK_COMDAT;

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "codegen-llvm/llvm_backend.h"
#include "il/il.h"
#include "support/diagnostics.h"

using namespace bronze;

TEST_CASE("LLVM backend metadata") {
    LLVMBackend backend;
    CHECK(std::string(backend.name()) == "llvm");
}

TEST_CASE("LLVM backend emits object file for arithmetic IL module") {
    il::Module module;
    module.name = "test_arithmetic";

    il::Function addFunc;
    addFunc.name = "add";
    addFunc.params = {{"a", il::Type::F64}, {"b", il::Type::F64}};
    addFunc.returnType = il::Type::F64;
    addFunc.isExported = false;
    addFunc.valueCount = 3;
    addFunc.blocks = {
        {
            0, {},
            {
                {il::Op::Add, il::Type::F64, 2, {0, 1}, 0.0, 0, 0},
                {il::Op::Ret, il::Type::F64, il::kNoValue, {2}, 0.0, 0, 0}
            }
        }
    };
    module.functions.push_back(addFunc);

    il::Function mainFunc;
    mainFunc.name = "main";
    mainFunc.params = {};
    mainFunc.returnType = il::Type::F64;
    mainFunc.isExported = true;
    mainFunc.valueCount = 8;
    mainFunc.blocks = {
        {
            0, {},
            {
                {il::Op::ConstF64, il::Type::F64, 0, {}, 10.0, 0, 0},
                {il::Op::ConstF64, il::Type::F64, 1, {}, 20.0, 0, 0},
                {il::Op::Mul, il::Type::F64, 2, {0, 1}, 0.0, 0, 0},
                {il::Op::ConstF64, il::Type::F64, 3, {}, 5.0, 0, 0},
                {il::Op::Sub, il::Type::F64, 4, {2, 3}, 0.0, 0, 0},
                {il::Op::ConstF64, il::Type::F64, 5, {}, 2.0, 0, 0},
                {il::Op::Div, il::Type::F64, 6, {4, 5}, 0.0, 0, 0},
                {il::Op::Call, il::Type::F64, 7, {6, 0}, 0.0, 0, 0},
                {il::Op::Ret, il::Type::F64, il::kNoValue, {7}, 0.0, 0, 0}
            }
        }
    };
    module.functions.push_back(mainFunc);

    std::filesystem::path outPath = std::filesystem::temp_directory_path() / "bronze_test_arith.obj";
    if (std::filesystem::exists(outPath)) {
        std::filesystem::remove(outPath);
    }

    DiagnosticSink diags;
    LLVMBackend backend;
    bool success = backend.emitObject(module, outPath.string(), diags);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(success);
    REQUIRE(std::filesystem::exists(outPath));
    CHECK(std::filesystem::file_size(outPath) > 0);

    std::filesystem::remove(outPath);
}

TEST_CASE("LLVM backend emits object file for comparison IL module") {
    il::Module module;
    module.name = "test_compare";

    il::Function cmpFunc;
    cmpFunc.name = "compare";
    cmpFunc.params = {{"a", il::Type::F64}, {"b", il::Type::F64}};
    cmpFunc.returnType = il::Type::Bool;
    cmpFunc.isExported = true;
    cmpFunc.valueCount = 5;
    cmpFunc.blocks = {
        {
            0, {},
            {
                {il::Op::CmpLt, il::Type::Bool, 2, {0, 1}, 0.0, 0, 0},
                {il::Op::CmpGt, il::Type::Bool, 3, {0, 1}, 0.0, 0, 0},
                {il::Op::CmpEq, il::Type::Bool, 4, {0, 1}, 0.0, 0, 0},
                {il::Op::Ret, il::Type::Bool, il::kNoValue, {2}, 0.0, 0, 0}
            }
        }
    };
    module.functions.push_back(cmpFunc);

    std::filesystem::path outPath = std::filesystem::temp_directory_path() / "bronze_test_cmp.obj";
    if (std::filesystem::exists(outPath)) {
        std::filesystem::remove(outPath);
    }

    DiagnosticSink diags;
    LLVMBackend backend;
    bool success = backend.emitObject(module, outPath.string(), diags);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(success);
    REQUIRE(std::filesystem::exists(outPath));
    CHECK(std::filesystem::file_size(outPath) > 0);

    std::filesystem::remove(outPath);
}

TEST_CASE("LLVM backend emits object file for dynamic IL module with Box, Unbox, PropGet, PropSet, DynamicCall") {
    il::Module module;
    module.name = "test_dynamic_ops";
    // Both property sites below name IC site 0, which the module has to
    // declare: the backend emits exactly icSiteCount entries as a global array,
    // so this is an allocation size, not a hint.
    module.icSiteCount = 1;

    il::Function dynFunc;
    dynFunc.name = "dynTest";
    dynFunc.params = {{"obj", il::Type::Dynamic}, {"fn", il::Type::Dynamic}};
    dynFunc.returnType = il::Type::Dynamic;
    dynFunc.isExported = true;
    dynFunc.valueCount = 8;
    dynFunc.blocks = {
        {
            0, {},
            {
                // %2: f64 = const.f64 123.0
                {il::Op::ConstF64, il::Type::F64, 2, {}, 123.0, 0, 0},
                // %3: dynamic = box.f64 %2
                {il::Op::Box, il::Type::Dynamic, 3, {2}, 0.0, 0, 0, il::Type::F64, 0, 0},
                // prop.set %0, 0, %3, 0
                {il::Op::PropSet, il::Type::Void, il::kNoValue, {0, 3}, 0.0, 0, 0, il::Type::Void, 0, 0},
                // %4: dynamic = prop.get %0, 0, 0
                {il::Op::PropGet, il::Type::Dynamic, 4, {0}, 0.0, 0, 0, il::Type::Void, 0, 0},
                // %5: f64 = unbox.f64 %4
                {il::Op::Unbox, il::Type::F64, 5, {4}, 0.0, 0, 0, il::Type::Void, 0, 0},
                // %6: dynamic = call.dynamic %1, %0, 1, %4
                {il::Op::DynamicCall, il::Type::Dynamic, 6, {1, 0, 4}, 0.0, 0, 0, il::Type::Void, 0, 0},
                // ret %6
                {il::Op::Ret, il::Type::Dynamic, il::kNoValue, {6}, 0.0, 0, 0}
            }
        }
    };
    module.functions.push_back(dynFunc);

    std::filesystem::path outPath = std::filesystem::temp_directory_path() / "bronze_test_dyn.obj";
    if (std::filesystem::exists(outPath)) {
        std::filesystem::remove(outPath);
    }

    DiagnosticSink diags;
    LLVMBackend backend;
    bool success = backend.emitObject(module, outPath.string(), diags);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(success);
    REQUIRE(std::filesystem::exists(outPath));
    CHECK(std::filesystem::file_size(outPath) > 0);

    std::filesystem::remove(outPath);
}

// A monomorphic property site emits a guarded fast path instead of a plain
// call, which SPLITS the basic block. The IL block therefore ends in a
// different LLVM block than it started in, and a terminator carrying block
// arguments has to name that one as the phi's predecessor — get it wrong and
// llvm::verifyModule rejects the module for a phi whose entries do not match
// its predecessors, which is what this pins. It also exercises the IC table
// global itself.
TEST_CASE("LLVM backend emits the inlined cache guard and keeps block-argument phis honest") {
    il::Module module;
    module.name = "test_inline_ic";
    module.icSiteCount = 2;

    il::Instruction monoGet;
    monoGet.op = il::Op::PropGet;
    monoGet.type = il::Type::Dynamic;
    monoGet.result = 1;
    monoGet.operands = {0};
    monoGet.keyIndex = 0;
    monoGet.icIndex = 0;
    monoGet.icMonomorphic = true;

    // A second site at the same receiver, unproven: the plain call form,
    // side by side with the inlined one in the same block.
    il::Instruction plainGet;
    plainGet.op = il::Op::PropGet;
    plainGet.type = il::Type::Dynamic;
    plainGet.result = 2;
    plainGet.operands = {0};
    plainGet.keyIndex = 0;
    plainGet.icIndex = 1;

    il::Instruction jump;
    jump.op = il::Op::Jump;
    jump.type = il::Type::Void;
    jump.result = il::kNoValue;
    jump.target = il::BlockTarget{1, {1, 2}};

    il::Instruction ret;
    ret.op = il::Op::Ret;
    ret.type = il::Type::Dynamic;
    ret.result = il::kNoValue;
    ret.operands = {3};

    il::Function fn;
    fn.name = "icTest";
    fn.params = {{"obj", il::Type::Dynamic}};
    fn.returnType = il::Type::Dynamic;
    fn.isExported = true;
    fn.valueCount = 5;
    fn.blocks = {
        {0, {}, {monoGet, plainGet, jump}},
        {1, {{3, il::Type::Dynamic}, {4, il::Type::Dynamic}}, {ret}},
    };
    module.functions.push_back(fn);

    std::filesystem::path outPath = std::filesystem::temp_directory_path() / "bronze_test_ic.obj";
    std::filesystem::remove(outPath);

    DiagnosticSink diags;
    LLVMBackend backend;
    bool success = backend.emitObject(module, outPath.string(), diags);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(success);
    REQUIRE(std::filesystem::exists(outPath));
    CHECK(std::filesystem::file_size(outPath) > 0);

    std::filesystem::remove(outPath);
}

// The IC table is a fixed-size global array in the object file, so a site index
// past the module's count would be an out-of-bounds store into the object
// file's own data. The verifier names it rather than the backend clamping it.
TEST_CASE("IL verification rejects a property site past the module's IC site count") {
    il::Module module;
    module.name = "test_ic_overrun";
    module.icSiteCount = 1;

    il::Instruction get;
    get.op = il::Op::PropGet;
    get.type = il::Type::Dynamic;
    get.result = 1;
    get.operands = {0};
    get.icIndex = 1;  // one past the end

    il::Instruction ret;
    ret.op = il::Op::Ret;
    ret.type = il::Type::Dynamic;
    ret.result = il::kNoValue;
    ret.operands = {1};

    il::Function fn;
    fn.name = "overrun";
    fn.params = {{"obj", il::Type::Dynamic}};
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 2;
    fn.blocks = {{0, {}, {get, ret}}};
    module.functions.push_back(fn);

    std::filesystem::path outPath = std::filesystem::temp_directory_path() / "bronze_test_ic_bad.obj";
    std::filesystem::remove(outPath);

    DiagnosticSink diags;
    LLVMBackend backend;
    CHECK_FALSE(backend.emitObject(module, outPath.string(), diags));
    CHECK(diags.hasErrors());
}

// The object exports bronze_main, the ABI fingerprint stamp, and NOTHING
// else. A program is compiled whole into one object, so no other function has
// a caller outside it — and a JS function's name is user-chosen text that
// must never meet the system linker's namespace. three.js r160 defines
// module-local functions named `bind` and `remove`; with external linkage
// they collided with ws2_32's and ucrt's exports of those names and the app
// failed to link (LNK2005). `bronze_object_abi_fingerprint` is the one other
// deliberate export: the runtime's program entry reads it (bronze_abi.h,
// "Drift between two BUILDS"), so it must be a global definition.
// Emits a two-function module under `entrySymbol` and returns every global
// symbol the object DEFINES. Shared by the two linkage cases below, because the
// claim they make is one claim about one emitter, asserted twice: once for the
// default entry a standalone program is linked through, and once for a named
// one, which is the case that decides whether two compiled modules can live in
// one image.
std::vector<std::string> definedExports(const std::string& entrySymbol,
                                        const char* objName) {
    il::Module module;
    module.name = "test_linkage";

    il::Function bindFunc;
    bindFunc.name = "bind";  // the ws2_32 collision, verbatim
    bindFunc.params = {};
    bindFunc.returnType = il::Type::F64;
    bindFunc.isExported = false;
    bindFunc.valueCount = 1;
    bindFunc.blocks = {
        {0, {}, {
            {il::Op::ConstF64, il::Type::F64, 0, {}, 7.0, 0, 0},
            {il::Op::Ret, il::Type::F64, il::kNoValue, {0}, 0.0, 0, 0}
        }}
    };
    module.functions.push_back(bindFunc);

    il::Function mainFunc;
    mainFunc.name = "main";
    mainFunc.params = {};
    mainFunc.returnType = il::Type::F64;
    mainFunc.isExported = true;
    mainFunc.valueCount = 1;
    mainFunc.blocks = {
        {0, {}, {
            {il::Op::Call, il::Type::F64, 0, {}, 0.0, 0, 0},
            {il::Op::Ret, il::Type::F64, il::kNoValue, {0}, 0.0, 0, 0}
        }}
    };
    module.functions.push_back(mainFunc);

    std::filesystem::path outPath = std::filesystem::temp_directory_path() / objName;
    std::filesystem::remove(outPath);

    DiagnosticSink diags;
    LLVMBackend backend;
    if (!entrySymbol.empty()) backend.setEntrySymbol(entrySymbol);
    REQUIRE(backend.emitObject(module, outPath.string(), diags));
    REQUIRE_FALSE(diags.hasErrors());

    std::vector<std::string> exports;

    auto binOrErr = llvm::object::ObjectFile::createObjectFile(outPath.string());
    REQUIRE(static_cast<bool>(binOrErr));
    auto* obj = llvm::dyn_cast<llvm::object::ObjectFile>(binOrErr->getBinary());
    REQUIRE(obj != nullptr);
    auto* coff = llvm::dyn_cast<llvm::object::COFFObjectFile>(obj);
    for (const llvm::object::SymbolRef& sym : obj->symbols()) {
        auto flagsOrErr = sym.getFlags();
        REQUIRE(static_cast<bool>(flagsOrErr));
        if (!(*flagsOrErr & llvm::object::SymbolRef::SF_Global)) continue;
        auto nameOrErr = sym.getName();
        REQUIRE(static_cast<bool>(nameOrErr));
        // External references (the ABI helpers `Call @0` may lean on) are
        // fine; a global DEFINITION other than the entry is the bug. Absolute
        // symbols are linker metadata (COFF's @feat.00), not exports.
        if (*flagsOrErr & llvm::object::SymbolRef::SF_Undefined) continue;
        if (*flagsOrErr & llvm::object::SymbolRef::SF_Absolute) continue;
        // COMDAT globals (constant pools like __real@…) deduplicate by
        // design; they are not in the linker's flat namespace the way a
        // plain global definition is.
        if (coff != nullptr) {
            auto secOrErr = sym.getSection();
            REQUIRE(static_cast<bool>(secOrErr));
            if (*secOrErr != coff->section_end()) {
                const llvm::object::coff_section* cs = coff->getCOFFSection(**secOrErr);
                if (cs->Characteristics & kCoffComdatFlag) continue;
            }
        }
        std::string symName(nameOrErr->str());
        if (symName.starts_with('_')) {
            symName = symName.substr(1);
        }
        exports.push_back(symName);
    }

    std::filesystem::remove(outPath);
    std::sort(exports.begin(), exports.end());
    return exports;
}

TEST_CASE("LLVM backend exports only the entry, the ABI stamp, and the manifest") {
    const std::vector<std::string> exports = definedExports({}, "bronze_test_linkage.obj");
    CAPTURE(exports);
    CHECK(exports == std::vector<std::string>{"bronze_main", "bronze_main_host_globals",
                                              "bronze_object_abi_fingerprint"});
}

// The same three exports under a name the caller chose, which is what makes
// more than one compiled module linkable into one image: the stamp and the
// host-globals manifest are named AFTER the entry, so none of an object's
// exports can collide with another object's. Everything else stays internal,
// so nothing else can either. (The default entry's stamp keeps its historical
// name, which rt.cpp and embed_run.cpp link against.)
TEST_CASE("a named entry symbol renames all of the object's exports") {
    const std::vector<std::string> exports =
        definedExports("bronze_module_a", "bronze_test_linkage_named.obj");
    CAPTURE(exports);
    CHECK(exports == std::vector<std::string>{"bronze_module_a",
                                              "bronze_module_a_abi_fingerprint",
                                              "bronze_module_a_host_globals"});
}
