#include <doctest/doctest.h>
#include "../test_temp_dir.h"
#include "codegen-brass/brass_backend.h"
#include "codegen-brass/brass_backend_debug.h"
#include "cli/driver.h"
#include "eval/eval.h"
#include "il/il.h"
#include "support/diagnostics.h"

#include <brass/debug/codeview_emitter.hpp>
#include <brass/debug/dwarf_emitter.hpp>
#include <brass/object/coff_writer.hpp>
#include <brass/object/elf_writer.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace bronze;

namespace {

uint16_t readU16Le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t readU32Le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

il::Module createTestModule(const std::string& filename = "test_source.js") {
    il::Module m;
    m.name = filename;
    m.sourceFiles = {filename};
    const std::string src =
        "function calculate(a, b) {\n"
        "    return a + b;\n"
        "}\n"
        "calculate(10, 20);\n";
    m.sourceTexts = {src};
    m.lineTables.emplace_back(src);

    il::Function calcFn;
    calcFn.name = "calculate";
    calcFn.sourceFile = 0;
    calcFn.sourceBegin = 0;
    calcFn.sourceEnd = 45;
    calcFn.params = {{"a", il::Type::F64}, {"b", il::Type::F64}};
    calcFn.returnType = il::Type::F64;
    calcFn.isExported = true;
    calcFn.valueCount = 3;

    il::Block b0;
    b0.id = 0;
    il::Instruction addInst;
    addInst.op = il::Op::Add;
    addInst.type = il::Type::F64;
    addInst.result = 2;
    addInst.operands = {0, 1};
    addInst.span = Span{32, 37, 0}; // line 2
    b0.instructions.push_back(addInst);

    il::Instruction retInst;
    retInst.op = il::Op::Ret;
    retInst.type = il::Type::Void;
    retInst.result = il::kNoValue;
    retInst.operands = {2};
    retInst.span = Span{25, 38, 0}; // line 2
    b0.instructions.push_back(retInst);

    calcFn.blocks.push_back(std::move(b0));
    m.functions.push_back(std::move(calcFn));

    il::Function mainFn;
    mainFn.name = "main";
    mainFn.isEntryPoint = true;
    mainFn.sourceFile = 0;
    mainFn.sourceBegin = 47;
    mainFn.sourceEnd = 66;
    mainFn.returnType = il::Type::Void;
    mainFn.valueCount = 3;

    il::Block mb0;
    mb0.id = 0;
    il::Instruction c0;
    c0.op = il::Op::ConstF64;
    c0.type = il::Type::F64;
    c0.result = 0;
    c0.immF64 = 10.0;
    c0.span = Span{57, 59, 0}; // line 4
    mb0.instructions.push_back(c0);

    il::Instruction c1;
    c1.op = il::Op::ConstF64;
    c1.type = il::Type::F64;
    c1.result = 1;
    c1.immF64 = 20.0;
    c1.span = Span{61, 63, 0}; // line 4
    mb0.instructions.push_back(c1);

    il::Instruction call;
    call.op = il::Op::Call;
    call.type = il::Type::F64;
    call.result = 2;
    call.calleeIndex = 0;
    call.operands = {0, 1};
    call.span = Span{47, 65, 0}; // line 4
    mb0.instructions.push_back(call);

    il::Instruction mret;
    mret.op = il::Op::Ret;
    mret.type = il::Type::Void;
    mret.result = il::kNoValue;
    mret.span = Span{47, 66, 0}; // line 4
    mb0.instructions.push_back(mret);

    mainFn.blocks.push_back(std::move(mb0));
    m.functions.push_back(std::move(mainFn));

    return m;
}

} // namespace

TEST_CASE("ELF ObjectFile contains DWARF debug sections when debug info enabled") {
    BrassBackend backend;
    backend.setTarget(brass::Target::x64_linux());
    backend.setEmitDebugInfo(true);
    DiagnosticSink diags;

    il::Module m = createTestModule("sample.js");
    auto obj = backend.buildObjectFile(m, diags);
    REQUIRE(!diags.hasErrors());
    REQUIRE(obj.has_value());

    const auto* lineSec = obj->get_section(".debug_line");
    const auto* infoSec = obj->get_section(".debug_info");
    const auto* abbrevSec = obj->get_section(".debug_abbrev");
    const auto* strSec = obj->get_section(".debug_str");

    REQUIRE(lineSec != nullptr);
    REQUIRE(infoSec != nullptr);
    REQUIRE(abbrevSec != nullptr);
    REQUIRE(strSec != nullptr);

    CHECK(!lineSec->data.empty());
    CHECK(!infoSec->data.empty());
    CHECK(!abbrevSec->data.empty());
    CHECK(!strSec->data.empty());

    // DWARF version 4 check in .debug_line header
    REQUIRE(lineSec->data.size() >= 6);
    uint16_t lineVer = readU16Le(lineSec->data.data() + 4);
    CHECK(lineVer == 4);

    // DWARF version 4 check in .debug_info header
    REQUIRE(infoSec->data.size() >= 6);
    uint16_t infoVer = readU16Le(infoSec->data.data() + 4);
    CHECK(infoVer == 4);

    // Verify ElfWriter emits successfully with DWARF sections
    brass::object::ElfWriter writer(*obj);
    std::vector<uint8_t> elfBytes = writer.write();
    CHECK(!elfBytes.empty());
    CHECK(elfBytes.size() > 500);
}

TEST_CASE("AArch64 ELF ObjectFile contains DWARF debug sections") {
    BrassBackend backend;
    backend.setTarget(brass::Target::aarch64_linux());
    backend.setEmitDebugInfo(true);
    DiagnosticSink diags;

    il::Module m = createTestModule("sample_aarch64.js");
    auto obj = backend.buildObjectFile(m, diags);
    REQUIRE(!diags.hasErrors());
    REQUIRE(obj.has_value());

    CHECK(obj->get_section(".debug_line") != nullptr);
    CHECK(obj->get_section(".debug_info") != nullptr);
    CHECK(obj->get_section(".debug_abbrev") != nullptr);
    CHECK(obj->get_section(".debug_str") != nullptr);

    brass::object::ElfWriter writer(*obj);
    std::vector<uint8_t> elfBytes = writer.write();
    CHECK(!elfBytes.empty());
}

TEST_CASE("Line entries map back to source lines and filenames") {
    BrassBackend backend;
    backend.setTarget(brass::Target::x64_linux());
    backend.setEmitDebugInfo(true);
    DiagnosticSink diags;

    const std::string filename = "math_ops.js";
    il::Module m = createTestModule(filename);
    auto obj = backend.buildObjectFile(m, diags);
    REQUIRE(!diags.hasErrors());
    REQUIRE(obj.has_value());

    // Verify debug_context contains the filename
    CHECK(obj->debug_context.file_count() >= 1);
    bool foundFileInCtx = false;
    uint32_t mathFileId = 0;
    for (size_t f = 1; f <= obj->debug_context.file_count(); ++f) {
        if (obj->debug_context.get_file(static_cast<uint32_t>(f)) == filename) {
            foundFileInCtx = true;
            mathFileId = static_cast<uint32_t>(f);
            break;
        }
    }
    CHECK(foundFileInCtx);
    REQUIRE(mathFileId > 0);

    // Verify function debug table mapping
    bool foundCalcTable = false;
    for (const auto& dt : obj->debug_tables) {
        if (dt.function_name() == "calculate") {
            foundCalcTable = true;
            CHECK(dt.decl_file() == mathFileId);
            CHECK(dt.decl_line() == 1);
            CHECK(!dt.line_entries().empty());

            // Check line entries map to valid source lines
            for (const auto& le : dt.line_entries()) {
                CHECK(le.loc.file_id == mathFileId);
                CHECK(le.loc.line >= 1);
            }
        }
    }
    CHECK(foundCalcTable);

    // Verify .debug_str contains filename and symbols
    const auto* strSec = obj->get_section(".debug_str");
    REQUIRE(strSec != nullptr);
    std::string_view strContent(reinterpret_cast<const char*>(strSec->data.data()), strSec->data.size());
    CHECK(strContent.find(filename) != std::string_view::npos);
    CHECK(strContent.find("calculate") != std::string_view::npos);
}

TEST_CASE("COFF ObjectFile contains CodeView debug sections when debug info enabled") {
    BrassBackend backend;
    backend.setTarget(brass::Target::x64_windows());
    backend.setEmitDebugInfo(true);
    DiagnosticSink diags;

    il::Module m = createTestModule("win_calc.js");
    auto obj = backend.buildObjectFile(m, diags);
    REQUIRE(!diags.hasErrors());
    REQUIRE(obj.has_value());

    const auto* sSec = obj->get_section(".debug$S");
    const auto* tSec = obj->get_section(".debug$T");

    REQUIRE(sSec != nullptr);
    REQUIRE(tSec != nullptr);

    CHECK(!sSec->data.empty());
    CHECK(!tSec->data.empty());

    // Verify CodeView C13 signature (4)
    REQUIRE(sSec->data.size() >= 4);
    uint32_t sigS = readU32Le(sSec->data.data());
    CHECK(sigS == brass::debug::codeview::CV_SIGNATURE_C13);

    REQUIRE(tSec->data.size() >= 4);
    uint32_t sigT = readU32Le(tSec->data.data());
    CHECK(sigT == brass::debug::codeview::CV_SIGNATURE_C13);

    // Verify COFF emission contains .debug$S and .debug$T sections
    brass::object::CoffWriter writer(*obj);
    std::vector<uint8_t> coffBytes = writer.write();
    REQUIRE(coffBytes.size() >= 20);

    const uint8_t* hdr = coffBytes.data();
    uint16_t numSec = readU16Le(hdr + 2);
    const uint8_t* secHdrs = hdr + 20;

    bool foundDebugS = false;
    bool foundDebugT = false;
    for (uint16_t i = 0; i < numSec; ++i) {
        const uint8_t* sh = secHdrs + i * 40;
        char name[9] = {0};
        std::memcpy(name, sh, 8);
        if (std::strcmp(name, ".debug$S") == 0) {
            foundDebugS = true;
            uint32_t rawSz = readU32Le(sh + 16);
            CHECK(rawSz > 0);
        } else if (std::strcmp(name, ".debug$T") == 0) {
            foundDebugT = true;
            uint32_t rawSz = readU32Le(sh + 16);
            CHECK(rawSz > 0);
        }
    }
    CHECK(foundDebugS);
    CHECK(foundDebugT);
}

TEST_CASE("AArch64 Windows COFF ObjectFile contains CodeView debug sections") {
    BrassBackend backend;
    backend.setTarget(brass::Target::aarch64_windows());
    backend.setEmitDebugInfo(true);
    DiagnosticSink diags;

    il::Module m = createTestModule("win_arm64.js");
    auto obj = backend.buildObjectFile(m, diags);
    REQUIRE(!diags.hasErrors());
    REQUIRE(obj.has_value());

    CHECK(obj->get_section(".debug$S") != nullptr);
    CHECK(obj->get_section(".debug$T") != nullptr);

    brass::object::CoffWriter writer(*obj);
    std::vector<uint8_t> coffBytes = writer.write();
    CHECK(!coffBytes.empty());
}

TEST_CASE("Stripped objects without -g / --debug do not bloat unnecessarily") {
    // 1. ELF verification
    {
        BrassBackend backendDebug;
        backendDebug.setTarget(brass::Target::x64_linux());
        backendDebug.setEmitDebugInfo(true);

        BrassBackend backendStripped;
        backendStripped.setTarget(brass::Target::x64_linux());
        backendStripped.setEmitDebugInfo(false);

        DiagnosticSink diagsDbg;
        DiagnosticSink diagsStr;
        il::Module mDbg = createTestModule("module.js");
        il::Module mStr = createTestModule("module.js");

        auto objDebug = backendDebug.buildObjectFile(mDbg, diagsDbg);
        auto objStripped = backendStripped.buildObjectFile(mStr, diagsStr);

        REQUIRE(objDebug.has_value());
        REQUIRE(objStripped.has_value());

        // Stripped object must have NO debug sections
        CHECK(objStripped->get_section(".debug_line") == nullptr);
        CHECK(objStripped->get_section(".debug_info") == nullptr);
        CHECK(objStripped->get_section(".debug_abbrev") == nullptr);
        CHECK(objStripped->get_section(".debug_str") == nullptr);
        CHECK(objStripped->get_section(".brass_dbg") == nullptr);

        // Debug object must have DWARF debug sections
        CHECK(objDebug->get_section(".debug_line") != nullptr);
        CHECK(objDebug->get_section(".debug_info") != nullptr);

        brass::object::ElfWriter writerDbg(*objDebug);
        brass::object::ElfWriter writerStr(*objStripped);

        std::vector<uint8_t> bytesDbg = writerDbg.write();
        std::vector<uint8_t> bytesStr = writerStr.write();

        CHECK(bytesDbg.size() > bytesStr.size());
    }

    // 2. COFF verification
    {
        BrassBackend backendDebug;
        backendDebug.setTarget(brass::Target::x64_windows());
        backendDebug.setEmitDebugInfo(true);

        BrassBackend backendStripped;
        backendStripped.setTarget(brass::Target::x64_windows());
        backendStripped.setEmitDebugInfo(false);

        DiagnosticSink diagsDbg;
        DiagnosticSink diagsStr;
        il::Module mDbg = createTestModule("module.js");
        il::Module mStr = createTestModule("module.js");

        auto objDebug = backendDebug.buildObjectFile(mDbg, diagsDbg);
        auto objStripped = backendStripped.buildObjectFile(mStr, diagsStr);

        REQUIRE(objDebug.has_value());
        REQUIRE(objStripped.has_value());

        // Stripped object must have NO CodeView sections
        CHECK(objStripped->get_section(".debug$S") == nullptr);
        CHECK(objStripped->get_section(".debug$T") == nullptr);
        CHECK(objStripped->get_section(".brass_dbg") == nullptr);

        // Debug object must have CodeView sections
        CHECK(objDebug->get_section(".debug$S") != nullptr);
        CHECK(objDebug->get_section(".debug$T") != nullptr);

        brass::object::CoffWriter writerDbg(*objDebug);
        brass::object::CoffWriter writerStr(*objStripped);

        std::vector<uint8_t> bytesDbg = writerDbg.write();
        std::vector<uint8_t> bytesStr = writerStr.write();

        CHECK(bytesDbg.size() > bytesStr.size());
    }
}

TEST_CASE("EvalOptions emitDebugInfo flag integration") {
    eval::EvalOptions dbgOpts;
    dbgOpts.filename = "eval_dbg.js";
    dbgOpts.emitDebugInfo = true;

    auto scriptDbg = eval::compileScript("function add(x, y) { return x + y; } add(3, 4);", dbgOpts);
    REQUIRE(scriptDbg != nullptr);
    CHECK(scriptDbg->success);

    eval::EvalOptions strOpts;
    strOpts.filename = "eval_str.js";
    strOpts.emitDebugInfo = false;

    auto scriptStr = eval::compileScript("function add(x, y) { return x + y; } add(3, 4);", strOpts);
    REQUIRE(scriptStr != nullptr);
    CHECK(scriptStr->success);
}

TEST_CASE("CLI runBuild emitDebugInfo flag integration") {
    namespace fs = std::filesystem;
    fs::path tempDir = bronze_test::tempDir() / "bronze_debug_cli_test";
    std::error_code ec;
    fs::create_directories(tempDir, ec);

    fs::path srcFile = tempDir / "cli_sample.js";
    {
        std::ofstream out(srcFile);
        out << "function foo(n) { return n * 3; }\nconsole.log(foo(10));\n";
    }

    fs::path outObjDbg = tempDir / "cli_sample_dbg.o";
    fs::path outObjStr = tempDir / "cli_sample_str.o";

    std::string errDbg;
    int resDbg = cli::runBuild(
        srcFile.string(), outObjDbg.string(), &errDbg,
        /*infer=*/true, /*timings=*/false, /*emitObj=*/true,
        /*hostGlobalsPath=*/{}, /*inferStats=*/false, /*statsOut=*/nullptr,
        /*moduleRoots=*/{}, /*entrySymbol=*/{}, /*emitShared=*/false,
        /*retainFnSource=*/true, /*importMapPath=*/{}, /*assumeNoBigInt=*/false,
        /*pinsPath=*/{}, /*censusOutPath=*/{}, /*pinsAllowObserved=*/false,
        /*nativeManifestPath=*/{}, /*nativeLibPath=*/{}, /*entryResolvesAs=*/{},
        /*targetName=*/"x64-linux", /*publishModules=*/false, /*emitDebugInfo=*/true
    );
    CHECK(resDbg == 0);
    CHECK(fs::exists(outObjDbg));

    std::string errStr;
    int resStr = cli::runBuild(
        srcFile.string(), outObjStr.string(), &errStr,
        /*infer=*/true, /*timings=*/false, /*emitObj=*/true,
        /*hostGlobalsPath=*/{}, /*inferStats=*/false, /*statsOut=*/nullptr,
        /*moduleRoots=*/{}, /*entrySymbol=*/{}, /*emitShared=*/false,
        /*retainFnSource=*/true, /*importMapPath=*/{}, /*assumeNoBigInt=*/false,
        /*pinsPath=*/{}, /*censusOutPath=*/{}, /*pinsAllowObserved=*/false,
        /*nativeManifestPath=*/{}, /*nativeLibPath=*/{}, /*entryResolvesAs=*/{},
        /*targetName=*/"x64-linux", /*publishModules=*/false, /*emitDebugInfo=*/false
    );
    CHECK(resStr == 0);
    CHECK(fs::exists(outObjStr));

    // The debug object must be larger than stripped object
    CHECK(fs::file_size(outObjDbg) > fs::file_size(outObjStr));

    fs::remove_all(tempDir, ec);
}
