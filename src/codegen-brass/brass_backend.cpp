#include "codegen-brass/brass_backend.h"
#include "codegen-brass/brass_backend_debug.h"
#include "codegen-brass/brass_backend_sections.h"
#include "codegen-brass/il_to_brass_ast.h"

#include "abi/bronze_abi.h"
#include "support/timings.h"

#include <brass/brass.hpp>
#include <brass/codegen/jit_exec.hpp>
#include "codegen-brass/il2mir/il_pipeline.h"
#include "codegen-brass/il2mir/il_translator.h"
#include <brass/object/coff_writer.hpp>
#include <brass/object/elf_writer.hpp>
#include <brass/object/macho_writer.hpp>
#include <brass/object/object_writer.hpp>
#include <brass/target/target.hpp>

#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace bronze {

namespace {

std::string sanitizeFunctionName(std::string_view name) {
    std::string s;
    s.reserve(name.size());
    for (char c : name) {
        s.push_back(c == ' ' ? '_' : c);
    }
    return s;
}

std::vector<std::string> computeUniqueFunctionNames(const il::Module& module) {
    const size_t numFns = module.functions.size();
    std::vector<std::string> uniqueNames(numFns);
    std::unordered_map<std::string, size_t> nameCounts;
    nameCounts.reserve(numFns);

    for (const auto& fn : module.functions) {
        nameCounts[sanitizeFunctionName(fn.name)]++;
    }

    std::unordered_set<std::string> usedNames;
    usedNames.reserve(numFns);

    for (size_t i = 0; i < numFns; ++i) {
        const auto& fn = module.functions[i];
        if (fn.isEntryPoint || (fn.name == "main" && nameCounts["main"] == 1)) {
            uniqueNames[i] = "main";
            usedNames.insert("main");
        }
    }

    for (size_t i = 0; i < numFns; ++i) {
        if (!uniqueNames[i].empty()) continue;
        const auto& fn = module.functions[i];
        std::string sName = sanitizeFunctionName(fn.name);
        if (nameCounts[sName] == 1 && !usedNames.contains(sName)) {
            usedNames.insert(sName);
            uniqueNames[i] = std::move(sName);
        } else {
            std::string uname = sName + "$" + std::to_string(i);
            usedNames.insert(uname);
            uniqueNames[i] = std::move(uname);
        }
    }

    return uniqueNames;
}


// The calling thread's address of one of the module's writable tables
// (bronze_abi.h, bronze_module_instance): the image symbol plus the thread's
// delta, read from `module_deltas[*__bronze_module_slot]` through the pinned
// TLS register, exactly as brass's lowering reads it for the functions it
// lowers. The thunks here are only ever called from compiled code of this
// module on a thread that already ran its entry, so the slot is registered.
brass::Value* moduleDataAddr(brass::Builder& b, const std::string& entrySymbol,
                             const std::string& tableSym, bool perThread) {
    brass::Value* addr = b.build_func_addr(tableSym);
    if (!perThread) return addr;
    brass::Value* tls = b.build_pinned_tls_read();
    brass::Value* deltas = b.build_load(brass::Type::i64(), tls, BRONZE_TLS_MODULE_DELTAS_OFF);
    brass::Value* slotCell = b.build_func_addr(codegen::moduleSymbolName(entrySymbol, "__bronze_module_slot"));
    brass::Value* slot = b.build_load(brass::Type::i64(), slotCell, 0);
    brass::Value* entry = b.build_add(deltas, b.build_shl(slot, b.build_iconst_i64(3)));
    brass::Value* delta = b.build_load(brass::Type::i64(), entry, 0);
    return b.build_add(addr, delta);
}

void emitGlobalReadThunks(brass::Module& mod, const std::string& entrySymbol,
                          const std::vector<uint32_t>& globalReadKeys, bool perThread) {
    if (globalReadKeys.empty()) return;

    const std::string globalCacheSym = codegen::moduleSymbolName(entrySymbol, "__bronze_global_cache");
    const std::string keyMapSym = codegen::moduleSymbolName(entrySymbol, "__bronze_key_map");
    const auto globalCacheCount = static_cast<int64_t>(globalReadKeys.size());

    mod.add_external_symbol(globalCacheSym);
    mod.add_external_symbol("bronze_global_get_cached");

    for (size_t slot = 0; slot < globalReadKeys.size(); ++slot) {
        const uint32_t key = globalReadKeys[slot];
        brass::Function* thunk = mod.create_function(codegen::globalReadThunkName(key), brass::Type::i64());
        brass::Builder b(mod);
        b.set_function(thunk);

        brass::BasicBlock* entry = b.append_block("entry");
        brass::BasicBlock* hit = b.append_block("hit");
        brass::BasicBlock* miss = b.append_block("miss");

        b.position_at_end(entry);
        brass::Value* cells = moduleDataAddr(b, entrySymbol, globalCacheSym, perThread);
        brass::Value* cached = b.build_load(brass::Type::i64(), cells, static_cast<int32_t>(slot * sizeof(uint64_t)));
        brass::Value* hole = b.build_iconst_i64(static_cast<int64_t>(BRONZE_ABI_HOLE_BITS));
        brass::Value* isHole = b.build_eq(cached, hole);
        b.build_br_if(isHole, miss, hit);

        b.position_at_end(hit);
        b.build_ret(cached);

        b.position_at_end(miss);
        brass::Value* keyMap = b.build_func_addr(keyMapSym);
        brass::Value* keyId = b.build_load(brass::Type::i32(), keyMap, static_cast<int32_t>(key * sizeof(uint32_t)));
        brass::Value* count = b.build_iconst_i64(globalCacheCount);
        brass::Value* slotVal = b.build_iconst_i32(static_cast<int32_t>(slot));
        brass::Value* resolved = b.build_call("bronze_global_get_cached", brass::Type::i64(), {keyId, cells, count, slotVal});
        b.build_ret(resolved);

        thunk->rebuild_cfg_predecessors();
    }
}

void emitNativeImportThunks(brass::Module& mod, const il::Module& module,
                            const std::string& entrySymbol, bool perThread) {
    const auto& imports = module.nativeImports;
    if (imports.empty()) return;

    const std::string importsSymbol = entrySymbol + "_native_imports";
    mod.add_external_symbol(importsSymbol);
    mod.add_external_symbol("bronze_native_bind");
    mod.add_external_symbol("bronze_native_unbound");
    mod.add_external_symbol("bronze_native_buffer_slot");
    mod.add_external_symbol("bronze_native_buffer_wrap");
    mod.add_external_symbol("bronze_native_buffer_abandon");

    for (size_t i = 0; i < imports.size(); ++i) {
        const il::Function& decl = module.functions[imports[i].functionIndex];
        const bool isClassSlot = imports[i].name.rfind("class ", 0) == 0;

        std::vector<brass::Type> paramTypes;
        paramTypes.reserve(decl.params.size());
        for (const auto& p : decl.params) {
            paramTypes.push_back(brassTypeOf(p.type));
        }

        const brass::Type retType = (decl.returnType == il::Type::Bool)
                                        ? brass::Type::i32()
                                        : brassTypeOf(decl.returnType);

        brass::Function* thunk = mod.get_function(decl.name);
        if (!thunk) {
            thunk = mod.create_function(
                decl.name, retType, brass::Span<const brass::Type>(paramTypes.data(), paramTypes.size()));
        }

        brass::Builder b(mod);
        b.set_function(thunk);
        brass::BasicBlock* entry = b.append_block("entry");

        std::vector<brass::Value*> args;
        args.reserve(paramTypes.size());
        for (const auto& t : paramTypes) {
            args.push_back(b.add_block_param(entry, t));
        }

        b.position_at_end(entry);
        brass::Value* table = moduleDataAddr(b, entrySymbol, importsSymbol, perThread);
        const auto slotOffset = static_cast<int32_t>(8 + i * sizeof(uint64_t));

        if (isClassSlot) {
            b.build_ret(b.build_load(brass::Type::i64(), table, slotOffset));
        } else if (imports[i].bufferReturnKind != UINT32_MAX) {
            // The native is called from a helper function the thunk invokes, so
            // a throw out of it lands at the thunk's pad, which pops the slot
            // (releasing a block the native had already given away) and raises
            // the value again. MIR has no indirect invoke; the helper is never
            // inlined at an invoke, since its indirect call could raise past
            // the pad.
            std::vector<brass::Type> innerTypes = paramTypes;
            innerTypes.push_back(brass::Type::ptr());
            const std::string innerName = decl.name + "__native_call";
            brass::Function* inner = mod.create_function(
                innerName, brass::Type::void_type(),
                brass::Span<const brass::Type>(innerTypes.data(), innerTypes.size()));
            {
                brass::Builder ib(mod);
                ib.set_function(inner);
                brass::BasicBlock* innerEntry = ib.append_block("entry");
                std::vector<brass::Value*> innerArgs;
                innerArgs.reserve(innerTypes.size());
                for (const auto& t : innerTypes) innerArgs.push_back(ib.add_block_param(innerEntry, t));
                ib.position_at_end(innerEntry);
                brass::Value* innerTable = moduleDataAddr(ib, entrySymbol, importsSymbol, perThread);
                brass::Value* callee = ib.build_load(brass::Type::ptr(), innerTable, slotOffset);
                ib.build_call_indirect(callee, brass::Type::void_type(),
                                       brass::Span<brass::Value* const>(innerArgs.data(), innerArgs.size()));
                ib.build_ret_void();
                inner->rebuild_cfg_predecessors();
            }

            brass::Value* slot = b.build_call("bronze_native_buffer_slot", brass::Type::ptr());
            std::vector<brass::Value*> withSlot = args;
            withSlot.push_back(slot);
            brass::BasicBlock* done = b.append_block("done");
            brass::BasicBlock* pad = b.append_block("pad");
            b.position_at_end(entry);
            b.build_invoke(innerName, brass::Type::void_type(),
                           brass::Span<brass::Value* const>(withSlot.data(), withSlot.size()), done, pad);

            b.position_at_end(done);
            brass::Value* kind = b.build_iconst_i32(static_cast<int32_t>(imports[i].bufferReturnKind));
            b.build_ret(b.build_call("bronze_native_buffer_wrap", brass::Type::i64(), {kind}));

            b.position_at_end(pad);
            brass::Value* thrown = b.build_landing_pad(brass::Type::i64());
            b.build_call("bronze_native_buffer_abandon", brass::Type::void_type());
            b.build_throw(thrown);
        } else {
            brass::Value* callee = b.build_load(brass::Type::ptr(), table, slotOffset);
            brass::Value* result = b.build_call_indirect(
                callee, retType, brass::Span<brass::Value* const>(args.data(), args.size()));

            if (retType.is_void()) {
                b.build_ret_void();
            } else if (decl.returnType == il::Type::Bool) {
                b.build_ret(b.build_and(result, b.build_iconst_i32(1)));
            } else {
                b.build_ret(result);
            }
        }
        thunk->rebuild_cfg_predecessors();
    }

    brass::Function* bind = mod.create_function("__bronze_native_bind", brass::Type::void_type());
    brass::Builder b(mod);
    b.set_function(bind);
    b.append_block("entry");
    // This thread's table: the entry binds the instance its registration
    // just made, from this thread's registry.
    brass::Value* table = moduleDataAddr(b, entrySymbol, importsSymbol, perThread);
    b.build_call("bronze_native_bind", brass::Type::void_type(), {table});
    b.build_ret_void();
    bind->rebuild_cfg_predecessors();
}

void renameEntrySymbol(brass::object::ObjectFile& obj, const std::string& entrySymbol) {
    if (entrySymbol.empty() || entrySymbol == "main") return;

    if (auto* sym = obj.find_symbol("main")) {
        sym->name = entrySymbol;
    }
    for (auto& cfi : obj.functions) {
        if (cfi.name == "main") {
            cfi.name = entrySymbol;
        }
    }
    for (auto& dt : obj.debug_tables) {
        if (dt.function_name() == "main") {
            dt.set_function_name(entrySymbol);
        }
    }
    for (auto& fn : obj.stack_maps.functions()) {
        if (fn.function_name == "main") {
            fn.function_name = entrySymbol;
        }
    }
    for (auto& sec : obj.sections) {
        for (auto& reloc : sec.relocations) {
            if (reloc.symbol_name == "main") {
                reloc.symbol_name = entrySymbol;
            }
        }
    }
}

} // namespace

void BrassBackend::setOptimizationOptions(il2mir::TranslatorOptions& options, bool optimize) const {
    options.enable_optimizations = optimize;
    options.enable_inlining = optimize;
    options.inline_leaf_only = true;
    options.enable_speculative_inlining = false;
    options.enable_parallel_loops = optimize;
    options.parallel_threshold = 1000;
    options.enable_sroa = true;
    options.enable_gvn = true;
    options.enable_sccp = true;
    options.enable_guard_elim = true;
    options.enable_cfg_simplify = true;
    options.enable_loop_unswitch = true;
    options.enable_jump_threading = true;
    options.enable_trace_layout = true;
    options.enable_f64_demote = true;
    options.enable_bce = true;
    options.enable_wbe = true;
    options.enable_gvn_pre = true;
    options.enable_loop_fusion = true;
    options.enable_loop_distribution = true;
    options.enable_array_contraction = true;
    options.enable_partial_escape = true;
    options.enable_allocation_sinking = true;
    // No FMA contraction, on any target: ECMA-262 Number arithmetic rounds
    // after every operation, and a fused a*b+c rounds once — a different
    // double (tests/oracle/cases/tiers_20_mat4_mul differs in the 13th digit).
    // brass's own default is right for its kernel users and wrong for JS.
    options.enable_fma = false;
    if (target_.is_aarch64()) {
        options.vector_width = 128;
    } else if (target_ == brass::Target::host() && target_.is_x64()) {
#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_cpu_supports("avx2")) {
            options.enable_avx2 = true;
            options.vector_width = 256;
        }
#endif
#endif
    }
}

brass::PassPipelineOptions BrassBackend::tierUpPasses() const {
    il2mir::TranslatorOptions options;
    setOptimizationOptions(options, /*optimize=*/true);
    return il2mir::pass_pipeline_options(options);
}

std::unique_ptr<brass::Module> BrassBackend::buildMirModule(
    const il::Module& module, DiagnosticSink& diags,
    std::vector<uint32_t>* globalReadKeysOut) {
    // One level inside buildObjectFile's "mir" phase: "translate" is brass's
    // IL lowering plus its whole MIR optimization pipeline.
    support::PhaseTimer mtimer(support::timingsEnabled(), 6);
    const std::vector<std::string> uniqueNames = computeUniqueFunctionNames(module);

    const bool optimize = this->optimize();
    il2mir::TranslatorOptions options;
    setOptimizationOptions(options, optimize);
    options.run_alias_analysis = optimize;
    options.enable_tlab = true;
    // The TLS block rides in a callee-saved register (bronze_abi_tls.h): the
    // entry loads it, the runtime's rtEnterJs trampoline loads it for every
    // other way in, and generated code reads the allocation window and the
    // stack limit through it without a call.
    options.pin_tls_register = true;
    options.key_constants = module.keyConstants;
    options.entry_symbol = entrySymbol_;
    options.enable_census = !module.censusSites.empty() && !module.censusOutPath.empty();
    options.census_site_count = static_cast<uint32_t>(module.censusSites.size());
    options.template_site_count = module.templateSiteCount;
    // One image, any number of threads: every writable table is addressed
    // through the calling thread's delta (bronze_abi.h,
    // bronze_module_instance), laid out by brass_backend_sections.cpp.
    options.per_thread_module_data = perThreadModuleData_;
    // The inline-cache table: lowering numbered every property and method
    // site, the verifier bounded each number, and `__bronze_ic_table`
    // (brass_backend_sections.cpp) is laid out to exactly this count, so brass
    // may address a site as `table + index * BRONZE_ABI_IC_SITE_SIZE` without
    // a check.
    options.ic_site_count = module.icSiteCount;
    const std::vector<uint32_t> methodIcSites = module.methodIcSites();
    options.method_ic_sites = methodIcSites;

    for (size_t i = 0; i < module.functions.size(); ++i) {
        const auto& fn = module.functions[i];
        il2mir::FunctionMeta meta;
        meta.needs_env = fn.needsEnv;
        meta.needs_this = fn.needsThis;
        meta.needs_arguments = fn.needsArguments;
        meta.has_rest_param = fn.hasRestParam;
        meta.is_strict = fn.isStrict;
        meta.first_source_param = static_cast<uint32_t>(fn.firstSourceParam());
        meta.fn_flags = fn.fnFlags | (fn.needsEnv ? 0x40u : 0u);
        meta.name_key = fn.nameKeyIndex;
        meta.required_args = fn.requiredArgs;
        meta.adapt_arity = fn.adaptArity();
        meta.params_pinned.reserve(fn.params.size());
        meta.param_pin_keys.reserve(fn.params.size());
        for (const auto& p : fn.params) {
            meta.params_pinned.push_back(p.pinned);
            meta.param_pin_keys.push_back(p.pinKeyIndex);
        }
        options.function_meta[uniqueNames[i]] = std::move(meta);
    }

    const size_t sourceFileCount = registerFnSources_ ? module.sourceTexts.size() : 0;
    options.source_files.reserve(sourceFileCount);
    for (uint16_t file = 0; file < sourceFileCount; ++file) {
        il2mir::TranslatorOptions::SourceFileMeta sf;
        sf.text_len = static_cast<uint32_t>(module.sourceTexts[file].size());
        for (size_t i = 0; i < module.functions.size(); ++i) {
            const auto& fn = module.functions[i];
            if (fn.sourceFile == file && fn.sourceEnd > fn.sourceBegin && uniqueNames[i] != "main") {
                sf.entry_count++;
            }
        }
        options.source_files.push_back(sf);
    }

    brass::DiagnosticReporter reporter;
    std::vector<uint32_t> globalReadKeys;
    mtimer.mark("options");
    auto ast = codegen::lowerToBrassAst(module, uniqueNames, &globalReadKeys);
    mtimer.mark("brass ast");
    il2mir::TranslationResult res = il2mir::translate_bronze_ast(ast, options, &reporter);
    mtimer.mark("translate");

    if (!res.success || !res.module || reporter.has_errors()) {
        if (reporter.has_errors() || reporter.has_warnings()) {
            for (const auto& diag : reporter.diagnostics()) {
                if (diag.severity == brass::DiagnosticSeverity::Error) {
                    diags.error(Span{}, diag.to_string());
                } else if (diag.severity == brass::DiagnosticSeverity::Warning) {
                    diags.warning(Span{}, diag.to_string());
                }
            }
        }
        if (!diags.hasErrors()) {
            std::string msg = !res.error_message.empty() ? res.error_message : "Failed to translate Bronze IL to Brass MIR";
            diags.error(Span{}, std::move(msg));
        }
        return nullptr;
    }

    if (entrySymbol_ != "main") {
        if (auto* fn = res.module->get_function("main")) {
            res.module->rename_function(fn, entrySymbol_);
        }
    }

    emitGlobalReadThunks(*res.module, entrySymbol_, globalReadKeys, perThreadModuleData_);
    emitNativeImportThunks(*res.module, module, entrySymbol_, perThreadModuleData_);
    mtimer.mark("thunks");

    if (globalReadKeysOut) {
        *globalReadKeysOut = std::move(globalReadKeys);
    }

    return std::move(res.module);
}

std::optional<brass::object::ObjectFile> BrassBackend::buildObjectFile(
    const il::Module& module, DiagnosticSink& diags) {
    // The inside of the CLI's "codegen" phase, one level deeper.
    support::PhaseTimer timer(support::timingsEnabled(), 4);
    const std::vector<std::string> uniqueNames = computeUniqueFunctionNames(module);
    std::vector<uint32_t> globalReadKeys;

    auto mirMod = buildMirModule(module, diags, &globalReadKeys);
    if (!mirMod) {
        return std::nullopt;
    }
    timer.mark("mir");

    const size_t globalCacheCount = globalReadKeys.size();
    const std::vector<uint32_t> methodIcSites = module.methodIcSites();
    const bool optimize = this->optimize();

    const brass::Target target = target_;
    brass::object::ModuleCompiler compiler(target);
    brass::codegen::SchedOptions schedOpts;
    schedOpts.enable_post_ra = optimize;
    schedOpts.enable_software_pipelining = optimize;
    compiler.set_sched_options(schedOpts);
    compiler.set_enable_trace_layout(optimize);
    compiler.set_enable_mir_opts(optimize);
    // Each function's map travels in its code range (brass_backend_sections.cpp);
    // the module-wide `__brass_stack_maps` blob would be a second copy.
    compiler.set_emit_stack_map_symbol(false);
    brass::object::ObjectFile obj = compiler.compile(*mirMod);
    timer.mark("brass compile");

    renameEntrySymbol(obj, entrySymbol_);

    // Everything the runtime reads by name beside brass's code: the rodata
    // tables, the descriptors and pc tables, the data cells and the import
    // table (brass_backend_sections.cpp).
    codegen::SectionInputs sectionInputs{module, uniqueNames, entrySymbol_, hostGlobals_,
                                         globalCacheCount, methodIcSites};
    codegen::translateDebugLocations(obj, module, uniqueNames, entrySymbol_);
    codegen::emitBronzeSections(obj, target, sectionInputs, timer);
    codegen::emitNativeDebugSections(obj, module, uniqueNames, entrySymbol_, emitDebugInfo_);
    timer.mark("native debug");

    return obj;
}

brass::object::ObjectFile BrassBackend::buildDataImage(const il::Module& module, size_t globalCacheCount) {
    support::PhaseTimer timer(support::timingsEnabled(), 4);
    const std::vector<std::string> uniqueNames = computeUniqueFunctionNames(module);
    const std::vector<uint32_t> methodIcSites = module.methodIcSites();
    brass::object::ObjectFile obj;
    obj.target = target_;
    obj.module_name = module.name;
    codegen::SectionInputs sectionInputs{module, uniqueNames, entrySymbol_, hostGlobals_,
                                         globalCacheCount, methodIcSites};
    codegen::emitBronzeSections(obj, target_, sectionInputs, timer);
    return obj;
}

bool BrassBackend::emitObject(const il::Module& module, const std::string& outputPath,
                              DiagnosticSink& diags) {
    auto obj = buildObjectFile(module, diags);
    if (!obj) {
        return false;
    }

    const brass::Target target = target_;
    std::error_code ec;
    std::filesystem::path outPath(outputPath);
    if (outPath.has_parent_path()) {
        std::filesystem::create_directories(outPath.parent_path(), ec);
    }

    bool writeSuccess = false;
    if (target.is_windows()) {
        brass::object::CoffWriter writer(*obj);
        writeSuccess = writer.write_to_file(outputPath);
    } else if (target.is_macos()) {
        brass::object::MachOWriter writer(*obj, machoVersion_);
        writeSuccess = writer.write_to_file(outputPath);
    } else {
        brass::object::ElfWriter writer(*obj);
        writeSuccess = writer.write_to_file(outputPath);
    }

    if (!writeSuccess) {
        diags.error(Span{}, "Failed to write object file to: " + outputPath);
        return false;
    }

    if (emittedPathsOut_) {
        emittedPathsOut_->push_back(outputPath);
    }

    return true;
}

}  // namespace bronze
