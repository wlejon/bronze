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
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>

#include "codegen-llvm/llvm_alias.h"
#include "codegen-llvm/llvm_backend.h"
#include "codegen-llvm/llvm_call.h"
#include "codegen-llvm/llvm_partition.h"
#include "codegen-llvm/llvm_abi.h"
#include "codegen-llvm/llvm_convert.h"
#include "codegen-llvm/llvm_env.h"
#include "codegen-llvm/llvm_frame.h"
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

// ---- the storage alias families (llvm_alias.h) ------------------------------
//
// The pass is what licenses LLVM to keep a cached control word across a store
// to a GC root slot. It is checked on a hand-built module rather than through
// the compiler, because what has to hold is a statement about POINTER
// PROVENANCE and the two shapes that matter — a stack slot and a module table
// — are three instructions each.

namespace {

// The scope name an access was claimed by, or "" for an untagged one.
std::string scopeNameOf(const llvm::Instruction& inst) {
    auto* list = inst.getMetadata(llvm::LLVMContext::MD_alias_scope);
    if (list == nullptr || list->getNumOperands() != 1) return {};
    auto* scope = llvm::dyn_cast<llvm::MDNode>(list->getOperand(0));
    if (scope == nullptr || scope->getNumOperands() == 0) return {};
    auto* name = llvm::dyn_cast<llvm::MDString>(scope->getOperand(0));
    return name ? name->getString().str() : std::string{};
}

// Does `inst`'s noalias list name a scope spelled `want`? This is the half of
// the claim GVN actually spends: a store's noalias list has to name the load's
// scope for the two to be provably disjoint.
bool noaliasNames(const llvm::Instruction& inst, llvm::StringRef want) {
    auto* list = inst.getMetadata(llvm::LLVMContext::MD_noalias);
    if (list == nullptr) return false;
    for (const llvm::MDOperand& op : list->operands()) {
        auto* scope = llvm::dyn_cast<llvm::MDNode>(op.get());
        if (scope == nullptr || scope->getNumOperands() == 0) continue;
        if (auto* name = llvm::dyn_cast<llvm::MDString>(scope->getOperand(0));
            name != nullptr && name->getString() == want) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("storage alias families are assigned by pointer provenance") {
    llvm::LLVMContext ctx;
    llvm::Module m("alias_test", ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    auto* table = new llvm::GlobalVariable(m, i64Ty, /*isConstant=*/false,
                                           llvm::GlobalValue::InternalLinkage,
                                           llvm::ConstantInt::get(i64Ty, 0),
                                           "__bronze_ic_table");
    // A global that is NOT one of the module's tables: nothing is claimed
    // about it, because "every internal global" is exactly the rule this pass
    // refuses to use.
    auto* stranger = new llvm::GlobalVariable(m, i64Ty, /*isConstant=*/false,
                                              llvm::GlobalValue::InternalLinkage,
                                              llvm::ConstantInt::get(i64Ty, 0), "not_a_table");

    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {ptrTy}, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::GlobalValue::ExternalLinkage, "f", m);
    llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx, "entry", fn);
    llvm::IRBuilder<> b(bb);

    llvm::Value* frame = b.CreateAlloca(llvm::ArrayType::get(i64Ty, 4), nullptr, "gcframe");
    llvm::Value* slot = b.CreateConstInBoundsGEP2_32(llvm::ArrayType::get(i64Ty, 4), frame, 0, 2);
    auto* rootStore = b.CreateAlignedStore(b.getInt64(7), slot, llvm::Align(8));
    auto* tableLoad = b.CreateAlignedLoad(i64Ty, table, llvm::Align(8));
    auto* strangerLoad = b.CreateAlignedLoad(i64Ty, stranger, llvm::Align(8));
    // A heap access, tagged by its emitter: the pass must leave the family
    // alone and only widen what it claims not to alias.
    auto* heapLoad = b.CreateAlignedLoad(i64Ty, fn->getArg(0), llvm::Align(8));
    codegen_llvm::tagObjectSlotAccess(heapLoad, ctx);
    b.CreateRetVoid();

    codegen_llvm::tagStackAndControlAccesses(m, /*tlsFn=*/nullptr);

    CHECK(scopeNameOf(*rootStore) == "StackFrame");
    CHECK(scopeNameOf(*tableLoad) == "ModuleTables");
    CHECK(scopeNameOf(*strangerLoad).empty());
    CHECK(scopeNameOf(*heapLoad) == "ObjectPropertySlots");

    // The claims that pay: the root-slot store does not alias a module table,
    // and a heap slot does not alias either storage family.
    CHECK(noaliasNames(*rootStore, "ModuleTables"));
    CHECK(noaliasNames(*rootStore, "ObjectPropertySlots"));
    CHECK(noaliasNames(*tableLoad, "StackFrame"));
    CHECK(noaliasNames(*heapLoad, "StackFrame"));
    CHECK(noaliasNames(*heapLoad, "ModuleTables"));
}

TEST_CASE("a frameless variant's region and block parameters carry the storage they name") {
    llvm::LLVMContext ctx;
    llvm::Module m("region_alias_test", ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    // (region, tls, an ordinary pointer). The third is the control: a pointer
    // parameter says nothing about its storage unless the variant's convention
    // says what it is.
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {ptrTy, ptrTy, ptrTy}, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::GlobalValue::InternalLinkage, "f.inl", m);
    fn->addParamAttr(0, llvm::Attribute::get(ctx, codegen_llvm::kRegionParamAttr));
    fn->addParamAttr(1, llvm::Attribute::get(ctx, codegen_llvm::kTlsParamAttr));

    llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx, "entry", fn);
    llvm::IRBuilder<> b(bb);
    auto* slotStore = b.CreateAlignedStore(
        b.getInt64(7), b.CreateConstInBoundsGEP1_32(i64Ty, fn->getArg(0), 2), llvm::Align(8));
    auto* cellLoad = b.CreateAlignedLoad(
        i64Ty,
        b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), fn->getArg(1), BRONZE_TLS_EXCEPTION_CELL_OFF),
        llvm::Align(8));
    auto* strangerLoad = b.CreateAlignedLoad(i64Ty, fn->getArg(2), llvm::Align(8));
    b.CreateRetVoid();

    codegen_llvm::tagStackAndControlAccesses(m, /*tlsFn=*/nullptr);

    // The claim the split has to keep: a slot reached through the region is the
    // same stack storage it was when the frame was the function's own, so it
    // still does not alias the control word beside it.
    CHECK(scopeNameOf(*slotStore) == "StackFrame");
    CHECK(scopeNameOf(*cellLoad) ==
          "TlsWord." + std::to_string(BRONZE_TLS_EXCEPTION_CELL_OFF / 8));
    CHECK(scopeNameOf(*strangerLoad).empty());
    CHECK(noaliasNames(*slotStore, scopeNameOf(*cellLoad)));
}

// ---- the region plan (llvm_frame.h) ----------------------------------------

namespace {

// One IL function that needs a root slot: a Dynamic parameter is pinned, so
// `planFrame` gives it a slot and the function has a frame to merge.
il::Function envTakingFunction(const std::string& name) {
    il::Function fn;
    fn.name = name;
    fn.params = {{"__env", il::Type::Dynamic}};
    fn.needsEnv = true;
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 2;
    il::Instruction ret;
    ret.op = il::Op::Ret;
    ret.type = il::Type::Dynamic;
    ret.result = il::kNoValue;
    ret.operands = {0};
    fn.blocks = {{0, {}, {ret}}};
    return fn;
}

// A sibling-closure edge from `fn` to function index `callee`, prepended to the
// body so the `Ret` stays last.
void addClosureEdge(il::Function& fn, uint32_t callee) {
    il::Instruction call;
    call.op = il::Op::Call;
    call.type = il::Type::Dynamic;
    call.result = fn.valueCount++;
    call.operands = {0};
    call.calleeIndex = callee;
    call.callEnvHops = 0;
    fn.blocks[0].instructions.insert(fn.blocks[0].instructions.begin(), call);
}

std::vector<codegen_llvm::FramePlan> planAll(const il::Module& module) {
    std::vector<codegen_llvm::FramePlan> plans;
    for (const il::Function& fn : module.functions) {
        plans.push_back(
            codegen_llvm::planFrame(fn, /*moduleHasNewTarget=*/false, codegen_llvm::planRepr(fn)));
    }
    return plans;
}

}  // namespace

TEST_CASE("a chain of inline edges nests into one frame, sized by the deepest region") {
    if (codegen_llvm::frameMergeDisabled()) return;
    il::Module module;
    module.functions.push_back(envTakingFunction("leaf"));
    module.functions.push_back(envTakingFunction("mid"));
    module.functions.push_back(envTakingFunction("top"));
    addClosureEdge(module.functions[1], 0);
    addClosureEdge(module.functions[2], 1);

    const std::vector<codegen_llvm::FramePlan> plans = planAll(module);
    const codegen_llvm::RegionPlan regions = codegen_llvm::planRegions(module, plans);

    CHECK(regions.isMerged(2, 1));
    CHECK(regions.isMerged(1, 0));
    CHECK(regions.isMergeTarget[0]);
    CHECK(regions.isMergeTarget[1]);
    // Nothing calls `top`, so nothing ever enters it frameless.
    CHECK_FALSE(regions.isMergeTarget[2]);
    // The chain is three frames deep and becomes one, sized for all three.
    CHECK(regions.totalSlots[0] == plans[0].ownSlots);
    CHECK(regions.totalSlots[1] == plans[1].ownSlots + plans[0].ownSlots);
    CHECK(regions.totalSlots[2] == plans[2].ownSlots + plans[1].ownSlots + plans[0].ownSlots);
}

TEST_CASE("two inline edges out of one caller share a region rather than stacking") {
    if (codegen_llvm::frameMergeDisabled()) return;
    il::Module module;
    module.functions.push_back(envTakingFunction("a"));
    module.functions.push_back(envTakingFunction("b"));
    module.functions.push_back(envTakingFunction("caller"));
    addClosureEdge(module.functions[2], 0);
    addClosureEdge(module.functions[2], 1);

    const std::vector<codegen_llvm::FramePlan> plans = planAll(module);
    const codegen_llvm::RegionPlan regions = codegen_llvm::planRegions(module, plans);

    // Two calls in one caller are sequential and never both in flight, which
    // is why the region is the MAX of the two and not their sum — the rule the
    // argv region already runs on.
    CHECK(regions.totalSlots[2] ==
          plans[2].ownSlots + std::max(plans[0].ownSlots, plans[1].ownSlots));
}

TEST_CASE("an inline edge that would close a cycle is refused") {
    if (codegen_llvm::frameMergeDisabled()) return;
    il::Module module;
    module.functions.push_back(envTakingFunction("ping"));
    module.functions.push_back(envTakingFunction("pong"));
    addClosureEdge(module.functions[0], 1);
    addClosureEdge(module.functions[1], 0);

    const std::vector<codegen_llvm::FramePlan> plans = planAll(module);
    const codegen_llvm::RegionPlan regions = codegen_llvm::planRegions(module, plans);

    // A region contains what nests inside it, and a recursive nest has no
    // finite size — so exactly one of the two edges survives and the other
    // site keeps the shape it had before regions existed.
    CHECK((regions.isMerged(0, 1) != regions.isMerged(1, 0)));
    for (size_t i = 0; i < module.functions.size(); ++i) {
        CHECK(regions.totalSlots[i] <= plans[0].ownSlots + plans[1].ownSlots);
    }
}

TEST_CASE("a direct self-call is never a region") {
    if (codegen_llvm::frameMergeDisabled()) return;
    il::Module module;
    module.functions.push_back(envTakingFunction("rec"));
    addClosureEdge(module.functions[0], 0);

    const std::vector<codegen_llvm::FramePlan> plans = planAll(module);
    const codegen_llvm::RegionPlan regions = codegen_llvm::planRegions(module, plans);
    CHECK_FALSE(regions.isMerged(0, 0));
    CHECK_FALSE(regions.isMergeTarget[0]);
    CHECK(regions.totalSlots[0] == plans[0].ownSlots);
}

// ---- the environment access-guard elision (llvm_env.h) ----------------------

TEST_CASE("environment access guards are armed unless the seam disarms them") {
    // The tripwires are on in every build this suite runs, which is the
    // property that matters: a lowering bug meets them here. The elided shape
    // is covered by running the correctness suites under
    // BRONZE_ELIDE_ENV_GUARDS=1, not by flipping a process-wide seam inside
    // one test and deciding the answer for every other test in this binary.
    CHECK(codegen_llvm::envAccessGuardsElided() ==
          (std::getenv("BRONZE_ELIDE_ENV_GUARDS") != nullptr &&
           std::string(std::getenv("BRONZE_ELIDE_ENV_GUARDS")) == "1"));
}

// ---- the shapes stage E2 emits (llvm_env.h, llvm_convert.h) -----------------
//
// Both are checked on a hand-built module for the same reason the alias
// families are: what has to hold is a statement about the SHAPE of the emitted
// control flow — how many merges an access carries, and where the failure edge
// goes — and reading it off two dozen instructions is exact where reading it
// off a compiled fixture is arithmetic on a count.

namespace {

struct EnvShapeFixture {
    llvm::LLVMContext ctx;
    llvm::Module m{"env_shape_test", ctx};
    codegen_llvm::AbiFns abi{};
    llvm::Function* fn = nullptr;

    EnvShapeFixture() {
        codegen_llvm::declareAbiSymbols(m, ctx, abi, /*sharedRuntime=*/false);
        auto* fnTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx),
            {llvm::Type::getInt64Ty(ctx), llvm::Type::getDoubleTy(ctx)}, false);
        fn = llvm::Function::Create(fnTy, llvm::GlobalValue::ExternalLinkage, "f", m);
    }

    // The callees of every block whose terminator is `unreachable`.
    std::vector<std::string> unreachableCallees() const {
        std::vector<std::string> out;
        for (const llvm::BasicBlock& bb : *fn) {
            if (!llvm::isa<llvm::UnreachableInst>(bb.getTerminator())) continue;
            for (const llvm::Instruction& inst : bb) {
                if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                    if (call->getCalledFunction() != nullptr) {
                        out.push_back(call->getCalledFunction()->getName().str());
                    }
                }
            }
        }
        return out;
    }

    size_t phiCount() const {
        size_t n = 0;
        for (const llvm::BasicBlock& bb : *fn) {
            for (const llvm::Instruction& inst : bb) {
                if (llvm::isa<llvm::PHINode>(&inst)) ++n;
            }
        }
        return n;
    }

    size_t opcodeCount(unsigned opcode) const {
        size_t n = 0;
        for (const llvm::BasicBlock& bb : *fn) {
            for (const llvm::Instruction& inst : bb) {
                if (inst.getOpcode() == opcode) ++n;
            }
        }
        return n;
    }

    std::vector<std::string> calleeNames() const {
        std::vector<std::string> out;
        for (const llvm::BasicBlock& bb : *fn) {
            for (const llvm::Instruction& inst : bb) {
                if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                    if (call->getCalledFunction() != nullptr) {
                        out.push_back(call->getCalledFunction()->getName().str());
                    }
                }
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }
};

}  // namespace

TEST_CASE("a guarded environment read with no TDZ is a load and a tripwire, with no merge") {
    // The seam column emits the pre-E2 merging shape on purpose.
    if (!codegen_llvm::envTripwireEdges()) return;
    EnvShapeFixture f;
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(f.ctx, "entry", f.fn);
    llvm::IRBuilder<> b(entry);
    codegen_llvm::ModuleTables tables{};

    llvm::Value* got =
        codegen_llvm::emitEnvGet(b, f.abi, tables, f.fn->getArg(0), /*depth=*/0, /*index=*/3,
                                 /*tdz=*/false, /*keyIndex=*/0, /*elideGuards=*/false);
    b.CreateRetVoid();

    // The read IS the load: nothing merges, because the only other edge out of
    // the guard does not come back.
    CHECK(llvm::isa<llvm::LoadInst>(got));
    CHECK(f.phiCount() == 0);
    CHECK(f.unreachableCallees() == std::vector<std::string>{"bronze_env_access_failed"});
    // And the fallback helper the merge used to call is gone from the shape.
    const std::vector<std::string> callees = f.calleeNames();
    CHECK(std::find(callees.begin(), callees.end(), "bronze_env_get") == callees.end());
}

TEST_CASE("a guarded environment write is one store, and its failure edge does not return") {
    // The seam column emits the pre-E2 merging shape on purpose.
    if (!codegen_llvm::envTripwireEdges()) return;
    EnvShapeFixture f;
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(f.ctx, "entry", f.fn);
    llvm::IRBuilder<> b(entry);

    codegen_llvm::emitEnvSet(b, f.abi, f.fn->getArg(0), /*depth=*/2, /*index=*/1, b.getInt64(0),
                             /*elideGuards=*/false, /*valueNeverPointer=*/false);
    b.CreateRetVoid();

    CHECK(f.opcodeCount(llvm::Instruction::Store) == 1);
    CHECK(f.phiCount() == 0);
    const std::vector<std::string> callees = f.calleeNames();
    CHECK(std::find(callees.begin(), callees.end(), "bronze_env_set") == callees.end());
    // Depth 2 is three records to brand-check plus the slot range, and every
    // one of those edges lands on the one tripwire.
    const std::vector<std::string> failed = f.unreachableCallees();
    CHECK(!failed.empty());
    for (const std::string& name : failed) {
        CHECK(name == "bronze_env_access_failed");
    }
}

TEST_CASE("the TDZ edge is the one that still merges, because raising returns") {
    // The seam column emits the pre-E2 merging shape on purpose.
    if (!codegen_llvm::envTripwireEdges()) return;
    EnvShapeFixture f;
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(f.ctx, "entry", f.fn);
    llvm::IRBuilder<> b(entry);
    codegen_llvm::ModuleTables tables{};

    llvm::Value* got =
        codegen_llvm::emitEnvGet(b, f.abi, tables, f.fn->getArg(0), /*depth=*/0, /*index=*/0,
                                 /*tdz=*/true, /*keyIndex=*/0, /*elideGuards=*/false);
    b.CreateRetVoid();

    auto* phi = llvm::dyn_cast<llvm::PHINode>(got);
    REQUIRE(phi != nullptr);
    CHECK(phi->getNumIncomingValues() == 2);
    CHECK(f.phiCount() == 1);
    // 9.1.1.1.6 is not a tripwire: this edge raises a ReferenceError the
    // program can catch, so it comes back to the merge. The access guard's
    // edge, in the same emission, still does not.
    const std::vector<std::string> callees = f.calleeNames();
    CHECK(std::find(callees.begin(), callees.end(), "bronze_env_get_tdz") != callees.end());
    CHECK(f.unreachableCallees() == std::vector<std::string>{"bronze_env_access_failed"});
}

TEST_CASE("ToInt32 of a double is a range test, an fptosi to i64 and a truncation") {
    EnvShapeFixture f;
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(f.ctx, "entry", f.fn);
    llvm::IRBuilder<> b(entry);

    llvm::Value* converted = codegen_llvm::emitToInt32F64(b, f.abi, f.fn->getArg(1));
    b.CreateRetVoid();

    REQUIRE(converted->getType()->isIntegerTy(32));
    if (!codegen_llvm::toInt32InlineEnabled()) return;  // the A/B seam column

    CHECK(f.phiCount() == 1);
    // The conversion goes through i64, not i32: that is what makes every wrap
    // between 2^31 and 2^63 an inline answer instead of a call.
    REQUIRE(f.opcodeCount(llvm::Instruction::FPToSI) == 1);
    for (const llvm::BasicBlock& bb : *f.fn) {
        for (const llvm::Instruction& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::FPToSI) {
                CHECK(inst.getType()->isIntegerTy(64));
            }
        }
    }
    // Both bounds tested with ORDERED predicates, which is what sends NaN out
    // through the same edge as the infinities.
    size_t ordered = 0;
    for (const llvm::BasicBlock& bb : *f.fn) {
        for (const llvm::Instruction& inst : bb) {
            if (const auto* cmp = llvm::dyn_cast<llvm::FCmpInst>(&inst)) {
                CHECK(llvm::FCmpInst::isOrdered(cmp->getPredicate()));
                ++ordered;
            }
        }
    }
    CHECK(ordered == 2);
    // The helper survives as the arm the range test refuses, and it is pure.
    CHECK(f.calleeNames() == std::vector<std::string>{"bronze_to_int32_f64"});
    CHECK(f.abi.bronze_to_int32_f64->doesNotAccessMemory() ==
          codegen_llvm::pureConversionHelpers());
}

// ---- the split's keep sets (llvm_partition.h) -------------------------------
//
// The bin packer is inlining-blind: it puts a direct-call callee wherever the
// instruction counts say, and every other bin turns that callee into a bare
// `declare` it cannot inline. Stage E4 measured that taking the inline away
// from `Matrix4.multiplyMatrices` — 16.25 -> 18.09 ns/call with neither
// function changed, only their bins. These check the repair on hand-built
// modules, because what has to hold is a statement about the PLAN and about
// LINKAGE, and both are a dozen instructions each.

namespace {

// A function of `insts` instructions returning its i64 argument, so its size
// is exactly what the packer will read.
llvm::Function* sizedFn(llvm::Module& m, llvm::StringRef name, unsigned insts) {
    llvm::LLVMContext& ctx = m.getContext();
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    auto* fn = llvm::Function::Create(llvm::FunctionType::get(i64Ty, {i64Ty}, false),
                                      llvm::GlobalValue::ExternalLinkage, name, m);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", fn));
    llvm::Value* v = fn->getArg(0);
    // One `ret` is the last of the `insts`; the rest are adds.
    for (unsigned i = 1; i < insts; ++i) v = b.CreateAdd(v, llvm::ConstantInt::get(i64Ty, 1));
    b.CreateRet(v);
    return fn;
}

// A direct call the way the backend emits one for a method edge: the metadata
// markDirectMethodInlining reads, and the `alwaysinline` that pass leaves.
void directCall(llvm::Function* caller, llvm::Function* callee) {
    llvm::BasicBlock& entry = caller->getEntryBlock();
    llvm::IRBuilder<> b(&entry, entry.begin());
    auto* call = b.CreateCall(callee, {caller->getArg(0)});
    call->setMetadata(codegen_llvm::kDirectMethodMD,
                      llvm::MDNode::get(caller->getContext(), {}));
    call->addFnAttr(llvm::Attribute::AlwaysInline);
}

unsigned binOfName(const codegen_llvm::PartitionPlan& plan, llvm::StringRef name) {
    auto it = plan.binOf.find(name.str());
    REQUIRE(it != plan.binOf.end());
    return it->second;
}

bool keptHere(const codegen_llvm::PartitionPlan& plan, unsigned bin, llvm::StringRef name) {
    REQUIRE(bin < plan.keepBodies.size());
    return plan.keepBodies[bin].count(name.str()) != 0;
}

}  // namespace

TEST_CASE("a direct-call callee in another bin is kept; an over-cap or uncalled one is not") {
    llvm::LLVMContext ctx;
    llvm::Module m("xpart_plan", ctx);
    // Sizes chosen so greedy largest-first into the least-loaded of three bins
    // is forced: 3000 -> b0, 2002 -> b1, 100 -> b2, 80 -> b2, which puts
    // `caller` apart from all three of its neighbours.
    llvm::Function* big = sizedFn(m, "callee_over_cap", 3000);
    llvm::Function* caller = sizedFn(m, "caller", 2000);
    llvm::Function* small = sizedFn(m, "callee_small", 100);
    sizedFn(m, "never_called", 80);
    directCall(caller, small);
    directCall(caller, big);

    const codegen_llvm::PartitionPlan plan = codegen_llvm::planPartitions(m, 3);
    const unsigned home = binOfName(plan, "caller");
    REQUIRE(binOfName(plan, "callee_small") != home);
    REQUIRE(binOfName(plan, "callee_over_cap") != home);
    REQUIRE(binOfName(plan, "never_called") != home);

    if (codegen_llvm::crossPartitionInlineCap() == 0) {  // the A/B seam column
        CHECK(!keptHere(plan, home, "callee_small"));
        return;
    }
    // The one edge the split would have broken.
    CHECK(keptHere(plan, home, "callee_small"));
    // 3000 instructions is past the cap: carrying it would cost every bin that
    // calls it a body the site's own budget refuses to inline anyway.
    CHECK(!keptHere(plan, home, "callee_over_cap"));
    // Nothing in this bin names it, so there is nothing here to inline it into.
    CHECK(!keptHere(plan, home, "never_called"));
    // And the bin that OWNS a body never "keeps" it — it emits it.
    CHECK(!keptHere(plan, binOfName(plan, "callee_small"), "callee_small"));
}

TEST_CASE("a call not marked for inlining does not drag a body across the split") {
    llvm::LLVMContext ctx;
    llvm::Module m("xpart_plain_call", ctx);
    llvm::Function* caller = sizedFn(m, "caller", 400);
    llvm::Function* plain = sizedFn(m, "plain_callee", 100);
    sizedFn(m, "filler", 300);
    {
        llvm::BasicBlock& entry = caller->getEntryBlock();
        llvm::IRBuilder<> b(&entry, entry.begin());
        b.CreateCall(plain, {caller->getArg(0)});  // no metadata, no alwaysinline
    }

    const codegen_llvm::PartitionPlan plan = codegen_llvm::planPartitions(m, 2);
    const unsigned home = binOfName(plan, "caller");
    REQUIRE(binOfName(plan, "plain_callee") != home);
    if (codegen_llvm::crossPartitionInlineCap() == 0) return;  // the A/B seam column
    // The shipped default keeps only what the compiler ASKED to have inlined.
    CHECK(keptHere(plan, home, "plain_callee") ==
          codegen_llvm::crossPartitionKeepsEveryDirectCall());
}

TEST_CASE("a chain of direct edges is followed to the shipped depth and no further") {
    llvm::LLVMContext ctx;
    llvm::Module m("xpart_chain", ctx);
    // 400 / 300 / 200 / 100 into four bins lands one per bin, in that order,
    // so a -> b -> c -> d crosses three splits.
    llvm::Function* a = sizedFn(m, "a", 400);
    llvm::Function* b = sizedFn(m, "b", 300);
    llvm::Function* c = sizedFn(m, "c", 200);
    llvm::Function* d = sizedFn(m, "d", 100);
    directCall(a, b);
    directCall(b, c);
    directCall(c, d);

    const codegen_llvm::PartitionPlan plan = codegen_llvm::planPartitions(m, 4);
    const unsigned home = binOfName(plan, "a");
    REQUIRE(binOfName(plan, "b") != home);
    REQUIRE(binOfName(plan, "c") != home);
    REQUIRE(binOfName(plan, "d") != home);
    if (codegen_llvm::crossPartitionInlineCap() == 0) return;  // the A/B seam column

    const unsigned depth = codegen_llvm::crossPartitionInlineDepth();
    CHECK(keptHere(plan, home, "b"));
    // The second hop is the whole reason depth is not fixed at 1: b's own body
    // would carry a `declare c` and the inline would stop one edge short.
    CHECK(keptHere(plan, home, "c") == (depth >= 2));
    CHECK(keptHere(plan, home, "d") == (depth >= 3));
}

TEST_CASE("a kept body is available_externally, is inlined, and is not emitted") {
    llvm::LLVMContext ctx;
    llvm::Module m("xpart_apply", ctx);
    llvm::Function* caller = sizedFn(m, "caller", 400);
    llvm::Function* small = sizedFn(m, "callee_small", 100);
    sizedFn(m, "never_called", 300);
    directCall(caller, small);

    const codegen_llvm::PartitionPlan plan = codegen_llvm::planPartitions(m, 2);
    const unsigned home = binOfName(plan, "caller");
    REQUIRE(binOfName(plan, "callee_small") != home);

    codegen_llvm::PartitionStats stats;
    REQUIRE(codegen_llvm::applyPartition(m, plan, home, stats).empty());

    llvm::Function* keptFn = m.getFunction("callee_small");
    REQUIRE(keptFn != nullptr);
    // A body nothing in this bin calls is a declaration, exactly as before.
    REQUIRE(m.getFunction("never_called") != nullptr);
    CHECK(m.getFunction("never_called")->isDeclaration());

    if (codegen_llvm::crossPartitionInlineCap() == 0) {  // the A/B seam column
        CHECK(keptFn->isDeclaration());
        return;
    }
    CHECK(keptFn->hasAvailableExternallyLinkage());
    CHECK(!keptFn->isDeclaration());
    // `available_externally` is a declaration to the linker, and the verifier
    // rejects dllexport on one.
    CHECK(keptFn->getDLLStorageClass() == llvm::GlobalValue::DefaultStorageClass);
    CHECK(stats.keptFns == 1);
    // 400 built plus the one call `directCall` inserted.
    CHECK(stats.ownInsts == 401);
    {
        std::string err;
        llvm::raw_string_ostream os(err);
        REQUIRE_FALSE(llvm::verifyModule(m, &os));
    }

    // The claim that matters is not the linkage, it is what the pipeline does
    // with it: the call goes away, and so does the body.
    llvm::PassBuilder pb;
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
    mpm.run(m, mam);

    llvm::Function* after = m.getFunction("callee_small");
    // EliminateAvailableExternallyPass drops the body back to a declaration
    // before the MC backend sees it, so this partition's object defines only
    // what its bin owns — which is what keeps the link free of duplicates.
    CHECK((after == nullptr || after->isDeclaration()));
    llvm::Function* callerAfter = m.getFunction("caller");
    REQUIRE(callerAfter != nullptr);
    size_t callsToKept = 0;
    for (const llvm::BasicBlock& bb : *callerAfter) {
        for (const llvm::Instruction& inst : bb) {
            if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
                if (call->getCalledFunction() == after) ++callsToKept;
            }
        }
    }
    CHECK(callsToKept == 0);
}

TEST_CASE("the plan is the same object twice over") {
    llvm::LLVMContext ctx;
    llvm::Module m("xpart_determinism", ctx);
    llvm::Function* caller = sizedFn(m, "caller", 400);
    llvm::Function* small = sizedFn(m, "callee_small", 100);
    // Same size, and the module walk sees `tie_b` first: only the sort's name
    // tie-break decides which bin each lands in.
    sizedFn(m, "tie_b", 200);
    sizedFn(m, "tie_a", 200);
    directCall(caller, small);

    const codegen_llvm::PartitionPlan first = codegen_llvm::planPartitions(m, 3);
    const codegen_llvm::PartitionPlan second = codegen_llvm::planPartitions(m, 3);
    CHECK(first.binOf == second.binOf);
    REQUIRE(first.keepBodies.size() == second.keepBodies.size());
    for (size_t i = 0; i < first.keepBodies.size(); ++i) {
        CHECK(first.keepBodies[i] == second.keepBodies[i]);
    }
    // Equal sizes are broken by NAME, not by whichever the walk saw first:
    // `tie_a` is packed before `tie_b` although the module lists it second.
    CHECK(binOfName(first, "tie_a") == 1);
    CHECK(binOfName(first, "tie_b") == 2);
}
