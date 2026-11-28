#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <brass/mir/block.hpp>
#include <brass/mir/builder.hpp>
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>
#include <brass/mir/types.hpp>
#include <doctest/doctest.h>

#include "embed/embed_native.h"
#include "eval/eval.h"

namespace {

std::atomic<uint64_t> gTestEpoch{1};

double testMirWeightedFallback(double scale, const float* data, uint32_t len) {
    if (!data || len == 0) return -1.0;
    return scale * static_cast<double>(data[0]) + 100.0;
}

void testBumpEpoch() {
    gTestEpoch.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<const brass::Module> buildWeightedMirModule() {
    auto mod = std::make_shared<brass::Module>("bronze_test_mir_weighted");
    brass::Function* fn = mod->create_function(
        "mir_weighted_f32",
        brass::Type::f64(),
        {brass::Type::f64(), brass::Type::i64(), brass::Type::i32()});

    brass::Builder b(*mod);
    b.set_function(fn);
    brass::BasicBlock* entry = b.append_block("entry");
    brass::BasicBlock* nonEmpty = b.append_block("non_empty");
    brass::BasicBlock* empty = b.append_block("empty");

    brass::Value* scale = b.add_block_param(entry, brass::Type::f64());
    brass::Value* ptr = b.add_block_param(entry, brass::Type::i64());
    brass::Value* len = b.add_block_param(entry, brass::Type::i32());

    b.position_at_end(entry);
    brass::Value* zero = b.build_iconst_i32(0);
    brass::Value* hasElem = b.build_sgt(len, zero);
    b.build_br_if(hasElem, nonEmpty, empty);

    b.position_at_end(nonEmpty);
    brass::Value* v0 = b.build_load(brass::Type::f32(), ptr, 0);
    brass::Value* d0 = b.build_fpext_f64_f32(v0);
    brass::Value* prod = b.build_mul(scale, d0);
    brass::Value* one = b.build_fconst_f64(1.0);
    b.build_ret(b.build_fadd(prod, one));

    b.position_at_end(empty);
    b.build_ret(b.build_fconst_f64(-1.0));

    fn->rebuild_cfg_predecessors();
    return mod;
}

void writeTextFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    REQUIRE(out.good());
}

}  // namespace

TEST_CASE("CustomModuleLoader loads non-JS ES imports and skips stale code cache") {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("bronze_custom_loader_" + std::to_string(stamp));
    const std::filesystem::path cacheDir = root / "cache";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(cacheDir);

    const std::filesystem::path mainPath = root / "main.js";
    const std::filesystem::path bkPath = root / "kernel.bk";
    const std::filesystem::path helperPath = root / "helper.bk";

    writeTextFile(mainPath,
                  "import { compute } from './kernel.bk';\n"
                  "import { bias } from './helper.bk';\n"
                  "globalThis.__bronzeCustomLoaderOut = compute(20) + bias();\n");
    writeTextFile(bkPath, "kernel v1");
    writeTextFile(helperPath, "helper v1");

    int multiplier = 2;
    bronze::eval::EvalOptions opts;
    opts.extraModuleExtensions = {".bk"};
    opts.codeCacheDir = cacheDir.string();
    opts.customModuleLoader = [&](const std::filesystem::path& resolvedPath) -> bronze::modules::CustomModuleResult {
        if (resolvedPath.extension() != ".bk") return {};
        bronze::modules::CustomModuleResult res;
        res.handled = true;
        res.extraFilesRead = {resolvedPath.string()};
        if (resolvedPath.filename() == "kernel.bk") {
            res.jsSource = "export function compute(x) { return x * " + std::to_string(multiplier) + "; }\n";
        } else {
            res.jsSource = "export function bias() { return 5; }\n";
        }
        return res;
    };

    auto readGlobalOut = []() -> bronze::Value {
        auto gt = bronze::embed::globalValue("globalThis");
        REQUIRE(gt.found);
        return bronze::embed::getProperty(gt.value, "__bronzeCustomLoaderOut");
    };

    auto r1 = bronze::eval::evalFile(mainPath.string(), opts);
    INFO("r1 error: ", bronze::embed::toUtf8(bronze::embed::getProperty(r1.value, "message")));
    REQUIRE_FALSE(r1.thrown);
    const bronze::Value out1 = readGlobalOut();
    REQUIRE(out1.isNumber());
    CHECK(out1.asNumber() == doctest::Approx(45.0));

    // Because dependencies used customLoader, the program must not be served stale from cache
    // when the loader generates new JS source for updated .bk inputs.
    multiplier = 3;
    writeTextFile(bkPath, "kernel v2 modified");
    auto r2 = bronze::eval::evalFile(mainPath.string(), opts);
    INFO("r2 error: ", bronze::embed::toUtf8(bronze::embed::getProperty(r2.value, "message")));
    REQUIRE_FALSE(r2.thrown);
    const bronze::Value out2 = readGlobalOut();
    REQUIRE(out2.isNumber());
    CHECK(out2.asNumber() == doctest::Approx(65.0));

    std::filesystem::remove_all(root);
}

TEST_CASE("registerNativeMir fuses unboxed MIR call in loop and falls back on epoch mismatch") {
    gTestEpoch.store(1, std::memory_order_relaxed);
    std::string err;

    bronze::embed::NativeSignature bumpSig;
    bumpSig.returnType = "void";
    REQUIRE(bronze::embed::registerNative(
        "testBumpEpoch", reinterpret_cast<void*>(&testBumpEpoch), bumpSig, &err));

    bronze::embed::NativeSignature sig;
    sig.returnType = "f64";
    sig.paramTypes = {"f64", "f32[]"};

    bronze::embed::NativeMirSpec spec;
    spec.mirModule = buildWeightedMirModule();
    spec.mirFunction = "mir_weighted_f32";
    spec.epochPtr = &gTestEpoch;
    spec.expectedEpoch = 1;
    spec.maxInlinedInsts = 2048;

    REQUIRE(bronze::embed::registerNativeMir(
        "testMirWeighted", reinterpret_cast<void*>(&testMirWeightedFallback), sig, spec, &err));

    const std::string src = R"JS(
        const arr = new Float32Array([4.0]);
        let before = 0;
        for (let i = 0; i < 200; i++) {
            before = testMirWeighted(3.0, arr);
        }
        testBumpEpoch();
        const after = testMirWeighted(3.0, arr);
        before * 1000 + after;
    )JS";

    auto res = bronze::eval::evalScript(src);
    REQUIRE_FALSE(res.thrown);
    REQUIRE(res.value.isNumber());
    // before == 3.0 * 4.0 + 1.0 = 13.0 (MIR path), after == 3.0 * 4.0 + 100.0 = 112.0 (C fallback)
    CHECK(res.value.asNumber() == doctest::Approx(13112.0));

    CHECK(bronze::embed::unregisterNative("testMirWeighted", bronze::embed::NativeKind::Function));
    CHECK(bronze::embed::unregisterNative("testBumpEpoch", bronze::embed::NativeKind::Function));
}
