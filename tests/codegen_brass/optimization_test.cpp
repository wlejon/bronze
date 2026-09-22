#include <doctest/doctest.h>

#include <brass/brass.hpp>
#include <brass/codegen/jit_exec.hpp>
#include <brass/codegen/lir.hpp>
#include <brass/mir/alias_analysis.hpp>
#include <brass/mir/builder.hpp>
#include <brass/mir/call_graph.hpp>
#include <brass/mir/gvn.hpp>
#include <brass/mir/inliner.hpp>
#include <brass/mir/loop_parallel.hpp>
#include <brass/mir/printer.hpp>
#include <brass/mir/verifier.hpp>
#include <brass/object/object_writer.hpp>
#include <brass/runtime/parallel_runtime.hpp>
#include <brass/target/aarch64/aarch64_isel.hpp>

#include "codegen-brass/brass_backend.h"
#include "codegen-brass/brass_jit.h"
#include "il/il.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/value.h"
#include "support/diagnostics.h"
#include <vector>

TEST_CASE("Brass MIR Alias Analysis - Improves Load and Store Elimination for Bronze Accesses") {
    // Check alias queries on Bronze object property slots, environment slots, and local allocs
    brass::Module mod("test_bronze_alias");
    brass::Builder b(mod);
    brass::Function* fn = mod.create_function("property_env_access", brass::Type::i64(), {
        brass::Type::ptr(), // arg0: obj_ptr (Bronze object with property slots)
        brass::Type::ptr()  // arg1: env_ptr (Bronze environment frame)
    });
    b.set_function(fn);

    brass::BasicBlock* entry = b.append_block("entry");
    b.position_at_end(entry);
    brass::Value* obj = b.add_block_param(entry, brass::Type::ptr());
    brass::Value* env = b.add_block_param(entry, brass::Type::ptr());
    obj->set_noalias(true);
    env->set_noalias(true);

    // Bronze property offsets: e.g. slot 0 at offset 24, slot 1 at offset 32
    // Bronze environment offsets: parent frame at offset 16, var0 at offset 24
    // 1. Store 100 to obj.prop0 (offset 24)
    brass::Instruction* st_obj_prop0 = b.build_store(brass::Type::i64(), obj, 24, b.build_iconst_i64(100));
    // 2. Store 200 to obj.prop1 (offset 32)
    brass::Instruction* st_obj_prop1 = b.build_store(brass::Type::i64(), obj, 32, b.build_iconst_i64(200));
    // 3. Load obj.prop0 (offset 24) -> should forward from st_obj_prop0 because st_obj_prop1 does not clobber it
    brass::Value* ld_obj_prop0 = b.build_load(brass::Type::i64(), obj, 24);
    brass::Instruction* ld_inst_prop0 = ld_obj_prop0->defining_instruction();

    // 4. Store to env var at offset 24 on distinct env pointer
    brass::Instruction* st_env_var = b.build_store(brass::Type::i64(), env, 24, b.build_iconst_i64(999));
    // 5. Load obj.prop1 (offset 32)
    brass::Value* ld_obj_prop1 = b.build_load(brass::Type::i64(), obj, 32);
    brass::Instruction* ld_inst_prop1 = ld_obj_prop1->defining_instruction();

    // 6. Dead store test: store to offset 40 followed by overwrite to offset 40 with no read
    b.build_store(brass::Type::i64(), obj, 40, b.build_iconst_i64(111));
    b.build_store(brass::Type::i64(), obj, 40, b.build_iconst_i64(222));

    brass::Value* sum = b.build_add(ld_obj_prop0, ld_obj_prop1);
    b.build_ret(sum);
    fn->rebuild_cfg_predecessors();

    brass::AliasAnalysis aa(*fn);

    // Disjoint property slots on same object: NoAlias
    CHECK(aa.alias(obj, 24, brass::Type::i64(), obj, 32, brass::Type::i64()) == brass::AliasResult::NoAlias);
    // Identical property slot on same object: MustAlias
    CHECK(aa.alias(obj, 24, brass::Type::i64(), obj, 24, brass::Type::i64()) == brass::AliasResult::MustAlias);
    // Object property vs distinct environment frame: NoAlias
    CHECK(aa.alias(obj, 24, brass::Type::i64(), env, 24, brass::Type::i64()) == brass::AliasResult::NoAlias);

    // Clobber checks:
    // Storing to prop1 (32) cannot clobber reading prop0 (24)
    CHECK(!aa.can_clobber(st_obj_prop1, ld_inst_prop0));
    // Storing to prop0 (24) clobbers reading prop0 (24)
    CHECK(aa.can_clobber(st_obj_prop0, ld_inst_prop0));
    // Storing to env (24) cannot clobber reading obj.prop1 (32)
    CHECK(!aa.can_clobber(st_env_var, ld_inst_prop1));

    // Run GVN with alias analysis options enabled
    brass::GvnStats stats;
    brass::GvnOptions gvn_opts;
    gvn_opts.stats = &stats;
    bool gvn_changed = brass::gvn_function(*fn, gvn_opts);
    CHECK(gvn_changed);
    CHECK(stats.loads_forwarded >= 1);
    CHECK(stats.dead_stores_eliminated >= 1);
}

TEST_CASE("Brass Backend - AArch64 SIMD Vectorization Config and Code Emission") {
    // 1. Verify BrassBackend configuration for AArch64 target
    bronze::BrassBackend backend;
    backend.setTarget(brass::Target::aarch64_linux());
    CHECK(backend.target().is_aarch64());

    // Build an IL module targeting AArch64
    bronze::il::Module m;
    m.name = "aarch64_target_test";
    bronze::il::Function fn;
    fn.name = "test_fn";
    fn.returnType = bronze::il::Type::F64;
    fn.params = {{"x", bronze::il::Type::F64}};
    fn.valueCount = 2;
    bronze::il::Block b0;
    b0.id = 0;
    b0.instructions.push_back({bronze::il::Op::Ret, bronze::il::Type::Void, bronze::il::kNoValue, {0}, 0, 0, 0});
    fn.blocks.push_back(std::move(b0));
    m.functions.push_back(std::move(fn));

    bronze::DiagnosticSink diags;
    auto obj = backend.buildObjectFile(m, diags);
    REQUIRE(!diags.hasErrors());
    REQUIRE(obj.has_value());
    CHECK(obj->target.is_aarch64());

    // 2. Test AArch64 128-bit SIMD Vector lowering and code emission
    brass::Module vmod("test_aarch64_simd");
    brass::Function* vfn = vmod.create_function("vector_math", brass::Type::f32x4(), {
        brass::Type::f32x4(), brass::Type::f32x4(), brass::Type::ptr()
    });
    brass::Builder vb(vmod);
    vb.set_function(vfn);
    brass::BasicBlock* ventry = vb.append_block("entry");
    vb.position_at_end(ventry);
    brass::Value* va = vb.add_block_param(ventry, brass::Type::f32x4());
    brass::Value* vb_val = vb.add_block_param(ventry, brass::Type::f32x4());
    brass::Value* vptr = vb.add_block_param(ventry, brass::Type::ptr());

    brass::Value* v_add = vb.build_vadd(va, vb_val);
    brass::Value* v_sub = vb.build_vsub(v_add, vb_val);
    brass::Value* v_mul = vb.build_vmul(v_sub, va);
    vb.build_vstore(brass::Type::f32x4(), vptr, v_mul);
    brass::Value* v_ld = vb.build_vload(brass::Type::f32x4(), vptr);
    vb.build_ret(v_ld);
    vfn->rebuild_cfg_predecessors();
    REQUIRE(brass::verify_function(*vfn));

    // Lower with AArch64ISel
    brass::aarch64::AArch64ISel isel(brass::Target::aarch64_linux(), brass::CallingConvention::aapcs64());
    auto lir = isel.lower(*vfn);
    REQUIRE(lir != nullptr);

    bool found_addps = false, found_subps = false, found_mulps = false;
    bool found_vec_mem = false;
    for (const auto& inst : lir->blocks[0]->instructions) {
        if (inst->opcode == brass::codegen::LirOpcode::Addps) found_addps = true;
        if (inst->opcode == brass::codegen::LirOpcode::Subps) found_subps = true;
        if (inst->opcode == brass::codegen::LirOpcode::Mulps) found_mulps = true;
        if (inst->opcode == brass::codegen::LirOpcode::Movaps || inst->opcode == brass::codegen::LirOpcode::Movups) {
            found_vec_mem = true;
        }
    }
    CHECK(found_addps);
    CHECK(found_subps);
    CHECK(found_mulps);
    CHECK(found_vec_mem);

    // Compile to AArch64 ObjectFile and verify machine code emission
    brass::object::ModuleCompiler compiler(brass::Target::aarch64_linux());
    brass::object::ObjectFile aarch64_obj = compiler.compile(vmod);
    CHECK(aarch64_obj.target.is_aarch64());
    const auto* text_sec = aarch64_obj.get_section(".text");
    REQUIRE(text_sec != nullptr);
    CHECK(!text_sec->data.empty());
}

TEST_CASE("Brass MIR - Affine Loop Auto-Parallelization Execution") {
    // 1. DOALL Vector Scale loop
    brass::Module mod("auto_par_mod");
    brass::Function* fn = mod.create_function("vec_scale", brass::Type::void_type(), {
        brass::Type::ptr(), brass::Type::ptr(), brass::Type::i64(), brass::Type::i64()
    });
    brass::Builder b(mod);
    b.set_function(fn);

    brass::BasicBlock* entry = b.append_block("entry");
    brass::Value* in_arr = b.add_block_param(entry, brass::Type::ptr());
    brass::Value* out_arr = b.add_block_param(entry, brass::Type::ptr());
    brass::Value* factor = b.add_block_param(entry, brass::Type::i64());
    brass::Value* n = b.add_block_param(entry, brass::Type::i64());
    in_arr->set_noalias(true);
    out_arr->set_noalias(true);
    b.position_at_end(entry);

    brass::BasicBlock* hdr = b.create_block("loop_hdr");
    brass::BasicBlock* body = b.create_block("loop_body");
    brass::BasicBlock* exit = b.create_block("exit");

    brass::Value* zero = b.build_iconst_i64(0);
    brass::Value* one = b.build_iconst_i64(1);
    b.build_br(hdr, {zero});

    fn->append_block(hdr);
    b.position_at_end(hdr);
    brass::Value* iv = b.add_block_param(hdr, brass::Type::i64());
    brass::Value* cond = b.build_slt(iv, n);
    b.build_br_if(cond, body, {}, exit, {});

    fn->append_block(body);
    b.position_at_end(body);
    brass::Value* elem = b.build_load_indexed(brass::Type::i64(), in_arr, iv, 8, 0);
    brass::Value* scaled = b.build_mul(elem, factor);
    b.build_store_indexed(brass::Type::i64(), out_arr, iv, 8, 0, scaled);
    brass::Value* next_iv = b.build_add(iv, one);
    b.build_br(hdr, {next_iv});

    fn->append_block(exit);
    b.position_at_end(exit);
    b.build_ret_void();

    fn->rebuild_cfg_predecessors();

    brass::DominatorTree dom(*fn);
    brass::ParallelLoopStats stats;
    brass::ParallelLoopOptions opts;
    opts.stats = &stats;
    opts.parallel_threshold = 100; // Low threshold for test execution

    bool changed = brass::auto_parallelize_function(*fn, dom, opts);
    CHECK(changed);
    CHECK_EQ(stats.parallel_loops_transformed, 1u);

    // Verify JIT execution
    brass::codegen::JitExecutionEngine jit(brass::Target::host());
    jit.register_external_symbol("brass_parallel_for", reinterpret_cast<void*>(&brass_parallel_for));
    jit.register_external_symbol("brass_parallel_for_chunks", reinterpret_cast<void*>(&brass_parallel_for));
    jit.register_external_symbol("brass_set_parallel_workers", reinterpret_cast<void*>(&brass_set_parallel_workers));
    jit.register_external_symbol("brass_get_parallel_workers", reinterpret_cast<void*>(&brass_get_parallel_workers));
    jit.register_external_symbol("brass_parallel_reduce_i64", reinterpret_cast<void*>(&brass_parallel_reduce_i64));
    jit.register_external_symbol("brass_parallel_reduce_f64", reinterpret_cast<void*>(&brass_parallel_reduce_f64));
    jit.register_external_symbol("brass_parallel_alloc_context", reinterpret_cast<void*>(&brass_parallel_alloc_context));
    jit.register_external_symbol("brass_parallel_free_context", reinterpret_cast<void*>(&brass_parallel_free_context));

    REQUIRE(jit.compile_and_load(mod));

    auto fn_ptr = jit.get_function_ptr<void(*)(int64_t*, int64_t*, int64_t, int64_t)>("vec_scale");
    REQUIRE(fn_ptr != nullptr);

    const int64_t N = 1000;
    std::vector<int64_t> in_data(N);
    std::vector<int64_t> out_data(N, 0);
    for (int64_t i = 0; i < N; ++i) {
        in_data[i] = i + 1;
    }

    brass_set_parallel_workers(4);
    fn_ptr(in_data.data(), out_data.data(), 5, N);

    for (int64_t i = 0; i < N; ++i) {
        CHECK_EQ(out_data[i], (i + 1) * 5);
    }
}

TEST_CASE("Brass Backend - Inlining IPO with Frame Safety and Unwinding Invariants") {
    brass::Module mod("test_inlining_safety");

    // 1. Leaf callee function: compute (a + b) * 3
    brass::Function* leaf_callee = mod.create_function("leaf_kernel", brass::Type::i64(), {
        brass::Type::i64(), brass::Type::i64()
    });
    {
        brass::Builder b(mod);
        b.set_function(leaf_callee);
        brass::BasicBlock* entry = b.append_block("entry");
        brass::Value* a = b.add_block_param(entry, brass::Type::i64());
        brass::Value* b_val = b.add_block_param(entry, brass::Type::i64());
        brass::Value* sum = b.build_add(a, b_val);
        brass::Value* three = b.build_iconst_i64(3);
        brass::Value* prod = b.build_mul(sum, three);
        b.build_ret(prod);
        leaf_callee->rebuild_cfg_predecessors();
    }

    // 2. Non-leaf callee function: calls external helper
    mod.create_function("dummy_helper", brass::Type::i64(), {brass::Type::i64()});
    brass::Function* non_leaf_callee = mod.create_function("non_leaf_callee", brass::Type::i64(), {brass::Type::i64()});
    {
        brass::Builder b(mod);
        b.set_function(non_leaf_callee);
        brass::BasicBlock* entry = b.append_block("entry");
        brass::Value* x = b.add_block_param(entry, brass::Type::i64());
        brass::Value* res = b.build_call("dummy_helper", brass::Type::i64(), {x});
        b.build_ret(res);
        non_leaf_callee->rebuild_cfg_predecessors();
    }

    // 3. Wrapper function (leaf, but name starts with __wrapper_)
    brass::Function* wrapper_fn = mod.create_function("__wrapper_foo", brass::Type::i64(), {brass::Type::i64()});
    {
        brass::Builder b(mod);
        b.set_function(wrapper_fn);
        brass::BasicBlock* entry = b.append_block("entry");
        brass::Value* x = b.add_block_param(entry, brass::Type::i64());
        brass::Value* res = b.build_add(x, b.build_iconst_i64(1));
        b.build_ret(res);
        wrapper_fn->rebuild_cfg_predecessors();
    }

    // 4. Caller calling all three
    brass::Function* caller = mod.create_function("caller_fn", brass::Type::i64(), {
        brass::Type::i64(), brass::Type::i64()
    });
    {
        brass::Builder b(mod);
        b.set_function(caller);
        brass::BasicBlock* entry = b.append_block("entry");
        brass::Value* x = b.add_block_param(entry, brass::Type::i64());
        brass::Value* y = b.add_block_param(entry, brass::Type::i64());
        brass::Value* v1 = b.build_call("leaf_kernel", brass::Type::i64(), {x, y});
        brass::Value* v2 = b.build_call("non_leaf_callee", brass::Type::i64(), {v1});
        brass::Value* v3 = b.build_call("__wrapper_foo", brass::Type::i64(), {v2});
        b.build_ret(v3);
        caller->rebuild_cfg_predecessors();
    }

    brass::CallGraph cg(mod);
    CHECK(cg.is_leaf(leaf_callee));
    CHECK(!cg.is_leaf(non_leaf_callee));
    CHECK(cg.is_leaf(wrapper_fn));

    // Run inliner with only_inline_leaf_functions = true
    brass::InlinerOptions inliner_opts;
    inliner_opts.only_inline_leaf_functions = true;

    bool inlined = brass::inline_module(mod, inliner_opts);
    CHECK(inlined);

    // Verify caller:
    // - Call to "leaf_kernel" was inlined (eliminated)
    // - Call to "non_leaf_callee" was NOT inlined (retained for frame safety)
    // - Call to "__wrapper_foo" was NOT inlined (retained for trampoline safety)
    bool has_leaf_call = false;
    bool has_non_leaf_call = false;
    bool has_wrapper_call = false;

    for (brass::BasicBlock* bb : caller->blocks()) {
        for (brass::Instruction* inst : *bb) {
            if (inst->opcode() == brass::Opcode::call) {
                std::string callee_name = std::string(inst->symbol());
                if (callee_name == "leaf_kernel") has_leaf_call = true;
                if (callee_name == "non_leaf_callee") has_non_leaf_call = true;
                if (callee_name == "__wrapper_foo") has_wrapper_call = true;
            }
        }
    }

    CHECK(!has_leaf_call);       // Successfully inlined!
    CHECK(has_non_leaf_call);    // Preserved for stack unwinding / frame inspection!
    CHECK(has_wrapper_call);     // Preserved for runtime wrappers!
}

TEST_CASE("Brass Backend - End-to-End IL JIT Execution with Optimizations Enabled") {
    bronze::BrassBackend backend;
    backend.setOptimize(true);
    CHECK(backend.optimize());

    bronze::il::Module m;
    m.name = "opt_jit_test";

    // helper(a: f64) -> a * 2.0
    bronze::il::Function helperFn;
    helperFn.name = "helper";
    helperFn.params = {{"a", bronze::il::Type::F64}};
    helperFn.returnType = bronze::il::Type::F64;
    helperFn.valueCount = 3;
    bronze::il::Block hb0;
    hb0.id = 0;
    hb0.instructions.push_back({bronze::il::Op::ConstF64, bronze::il::Type::F64, 1, {}, 2.0, 0, 0});
    hb0.instructions.push_back({bronze::il::Op::Mul, bronze::il::Type::F64, 2, {0, 1}, 0, 0, 0});
    hb0.instructions.push_back({bronze::il::Op::Ret, bronze::il::Type::F64, bronze::il::kNoValue, {2}, 0, 0, 0});
    helperFn.blocks.push_back(std::move(hb0));
    m.functions.push_back(std::move(helperFn));

    // compute(x: f64, y: f64) -> helper(x) + y
    bronze::il::Function compFn;
    compFn.name = "compute";
    compFn.isExported = true;
    compFn.params = {{"x", bronze::il::Type::F64}, {"y", bronze::il::Type::F64}};
    compFn.returnType = bronze::il::Type::F64;
    compFn.valueCount = 4;
    bronze::il::Block cb0;
    cb0.id = 0;
    // %2 = call @helper(%0)
    bronze::il::Instruction callInst;
    callInst.op = bronze::il::Op::Call;
    callInst.type = bronze::il::Type::F64;
    callInst.result = 2;
    callInst.operands = {0};
    callInst.calleeIndex = 0;
    cb0.instructions.push_back(callInst);
    // %3 = add %2, %1
    cb0.instructions.push_back({bronze::il::Op::Add, bronze::il::Type::F64, 3, {2, 1}, 0, 0, 0});
    cb0.instructions.push_back({bronze::il::Op::Ret, bronze::il::Type::F64, bronze::il::kNoValue, {3}, 0, 0, 0});
    compFn.blocks.push_back(std::move(cb0));
    m.functions.push_back(std::move(compFn));

    // main() -> void (entry point that initializes TLS and runtime register)
    bronze::il::Function mainFn;
    mainFn.name = "main";
    mainFn.isEntryPoint = true;
    mainFn.returnType = bronze::il::Type::Void;
    mainFn.valueCount = 1;
    bronze::il::Block mb0;
    mb0.id = 0;
    mb0.instructions.push_back({bronze::il::Op::Ret, bronze::il::Type::Void, bronze::il::kNoValue, {}, 0, 0, 0});
    mainFn.blocks.push_back(std::move(mb0));
    m.functions.push_back(std::move(mainFn));

    bronze::DiagnosticSink diags;
    auto program = backend.compileToJit(m, diags);
    REQUIRE(!diags.hasErrors());
    REQUIRE(program != nullptr);

    bronze::ShadowStackFrame gcFrame;
    program->run();

    void* wrapper = program->symbolAddress("__wrapper_compute");
    REQUIRE(wrapper != nullptr);
    auto wrapperCode = reinterpret_cast<bronze_fn_code>(wrapper);

    uint64_t args1[2] = { bronze::Value::fromDouble(21.0).rawBits(), bronze::Value::fromDouble(0.0).rawBits() };
    uint64_t res1 = bronze::rtEnterJs(wrapperCode, 0, 0, 2, args1);
    CHECK(bronze::Value::fromRawBits(res1).asNumber() == doctest::Approx(42.0));

    uint64_t args2[2] = { bronze::Value::fromDouble(5.0).rawBits(), bronze::Value::fromDouble(7.5).rawBits() };
    uint64_t res2 = bronze::rtEnterJs(wrapperCode, 0, 0, 2, args2);
    CHECK(bronze::Value::fromRawBits(res2).asNumber() == doctest::Approx(17.5));
}

