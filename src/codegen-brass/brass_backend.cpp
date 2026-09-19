#include "codegen-brass/brass_backend.h"
#include "codegen-brass/brass_backend_sections.h"
#include "codegen-brass/il_to_brass_ast.h"

#include "abi/bronze_abi.h"
#include "support/timings.h"

#include <brass/brass.hpp>
#include <brass/codegen/jit_exec.hpp>
#include <brass/il_translator/il_translator.hpp>
#include <brass/object/coff_writer.hpp>
#include <brass/object/elf_writer.hpp>
#include <brass/object/macho_writer.hpp>
#include <brass/object/object_writer.hpp>
#include <brass/target/target.hpp>

#include <cstdlib>
#include <filesystem>

namespace bronze {

bool BrassBackend::optimize() const {
    static const bool forcedOff = std::getenv("BRONZE_NO_OPT") != nullptr;
    return optimize_ && !forcedOff;
}

std::optional<brass::object::ObjectFile> BrassBackend::buildObjectFile(
    const il::Module& module, DiagnosticSink& diags) {
    // The inside of the CLI's "codegen" phase, one level deeper.
    support::PhaseTimer timer(support::timingsEnabled(), 4);
    std::vector<std::string> uniqueNames(module.functions.size());
    std::unordered_map<std::string, size_t> nameCounts;
    auto sanitizeName = [](std::string n) {
        for (char& c : n) {
            if (c == ' ') c = '_';
        }
        return n;
    };
    for (const auto& fn : module.functions) {
        nameCounts[sanitizeName(fn.name)]++;
    }
    std::unordered_set<std::string> usedNames;
    for (size_t i = 0; i < module.functions.size(); ++i) {
        const auto& fn = module.functions[i];
        if (fn.isEntryPoint || (fn.name == "main" && nameCounts["main"] == 1)) {
            uniqueNames[i] = "main";
            usedNames.insert("main");
        }
    }
    for (size_t i = 0; i < module.functions.size(); ++i) {
        if (!uniqueNames[i].empty()) continue;
        const auto& fn = module.functions[i];
        std::string sName = sanitizeName(fn.name);
        if (nameCounts[sName] == 1 && !usedNames.count(sName)) {
            uniqueNames[i] = sName;
            usedNames.insert(sName);
        } else {
            std::string uname = sName + "$" + std::to_string(i);
            uniqueNames[i] = uname;
            usedNames.insert(uname);
        }
    }

    const bool optimize = this->optimize();
    brass::il::TranslatorOptions options;
    options.enable_optimizations = optimize;
    options.enable_inlining = false;
    options.enable_speculative_inlining = false;
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
    options.enable_tlab = true;
    options.use_bronze_tlab = true;
    // The TLS block rides in a callee-saved register (bronze_abi_tls.h): the
    // entry loads it, the runtime's rtEnterJs trampoline loads it for every
    // other way in, and generated code reads the exception cell, the
    // allocation window and the stack limit through it without a call.
    options.pin_tls_register = true;
#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_cpu_supports("avx2")) {
        options.enable_avx2 = true;
        options.vector_width = 256;
    }
    options.enable_fma = true;
#endif
#endif
    options.enable_pic = sharedRuntime_;
    options.key_constants = module.keyConstants;
    options.entry_symbol = entrySymbol_;
    options.propagate_exceptions_in_entry = propagateExceptionsInEntry_;
    options.enable_census = !module.censusSites.empty() && !module.censusOutPath.empty();
    options.census_site_count = static_cast<uint32_t>(module.censusSites.size());
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
        brass::il::FunctionMeta meta;
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
        for (const auto& p : fn.params) {
            meta.params_pinned.push_back(p.pinned);
            meta.param_pin_keys.push_back(p.pinKeyIndex);
        }
        options.function_meta[uniqueNames[i]] = meta;
    }
    for (uint16_t file = 0; file < module.sourceTexts.size(); ++file) {
        brass::il::TranslatorOptions::SourceFileMeta sf;
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
    auto ast = codegen::lowerToBrassAst(module, uniqueNames, &globalReadKeys);
    timer.mark("il->ast");
    brass::il::TranslationResult res = brass::il::translate_bronze_ast(ast, options, &reporter);
    timer.mark("translate");
    if (!res.success || !res.module || reporter.has_errors()) {
        std::string msg = reporter.has_errors() ? reporter.format_all() : res.error_message;
        if (msg.empty()) {
            msg = "Failed to translate Bronze IL to Brass MIR";
        }
        diags.error(Span{}, msg);
        return std::nullopt;
    }

    if (entrySymbol_ != "main") {
        if (auto* fn = res.module->get_function("main")) {
            fn->set_name(res.module->string_pool().intern(entrySymbol_));
        }
    }

    auto moduleSym = [&](const std::string& base) -> std::string {
        return codegen::moduleSymbolName(entrySymbol_, base);
    };

    // Host-global read cache. One i64 cell per distinct key the module reads,
    // in `__bronze_global_cache` (module-suffixed like the key map), every
    // cell born as the hole. Each `global.get` calls the thunk for its key:
    //
    //   __bronze_global_read_k<k>():
    //       v = cache[slot]
    //       if v != HOLE: return v
    //       return bronze_global_get_cached(keyMap[k], cache, count, slot)
    //
    // The runtime registers the cell array as a module root span on first
    // sight, fills the slot for builtin and host answers, and pours the hole
    // back into every registered cell when registerGlobal replaces a name —
    // so the fast path is a load and a compare, and the resolve rules stay
    // entirely in bronze_global_get_cached.
    const std::string globalCacheSym = moduleSym("__bronze_global_cache");
    const size_t globalCacheCount = globalReadKeys.size();
    {
        brass::Module& mod = *res.module;
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
            brass::Value* cells = b.build_func_addr(globalCacheSym);
            brass::Value* cached = b.build_load(brass::Type::i64(), cells, static_cast<int32_t>(slot * sizeof(uint64_t)));
            brass::Value* hole = b.build_iconst_i64(static_cast<int64_t>(BRONZE_ABI_NO_EXCEPTION_BITS));
            brass::Value* isHole = b.build_eq(cached, hole);
            b.build_br_if(isHole, miss, hit);

            b.position_at_end(hit);
            b.build_ret(cached);

            b.position_at_end(miss);
            brass::Value* keyMap = b.build_func_addr(moduleSym("__bronze_key_map"));
            brass::Value* keyId = b.build_load(brass::Type::i32(), keyMap, static_cast<int32_t>(key * sizeof(uint32_t)));
            brass::Value* count = b.build_iconst_i64(static_cast<int64_t>(globalCacheCount));
            brass::Value* slotVal = b.build_iconst_i32(static_cast<int32_t>(slot));
            brass::Value* resolved = b.build_call("bronze_global_get_cached", brass::Type::i64(), {keyId, cells, count, slotVal});
            b.build_ret(resolved);
            thunk->rebuild_cfg_predecessors();
        }
    }

    // The native import table and the thunks that call through it
    // (bronze_abi.h, `<entry>_native_imports`). Lowering declared one external
    // IL function per import, `__bronze_native_<i>`, typed as the native's C
    // signature; each becomes
    //
    //   __bronze_native_<i>(args...):  callee = table.slots[i]; return callee(args...)
    //   __bronze_native_<i>():         return table.slots[i]          (a class tag)
    //   __bronze_native_bind():        bronze_native_bind(&table)
    //
    // so a call site is one load and one indirect call, and NOTHING in the
    // object names a native's own symbol. A returned C `bool` is masked to
    // its low bit: the ABI defines only `al` for it and the thunk's caller
    // reads an i32. A native answering `T[]` (bufferReturnKind set) is the
    // one thunk with more in it:
    //
    //   __bronze_native_<i>(args...):  slot = bronze_native_buffer_slot();
    //                                  callee(args..., slot);
    //                                  return bronze_native_buffer_wrap(kind)
    //
    // The IL still sees a Dynamic-returning call; the descriptor and the
    // wrap are sequenced here, identically for the JIT and for an emitted
    // object, and the exception check the IL places after the call is what
    // unwinds when the native threw (wrap answered undefined, releasing a
    // transferred block first).
    const std::string importsSymbol = entrySymbol_ + "_native_imports";
    const auto& imports = module.nativeImports;
    {
        brass::Module& mod = *res.module;
        mod.add_external_symbol(importsSymbol);
        mod.add_external_symbol("bronze_native_bind");
        mod.add_external_symbol("bronze_native_unbound");
        mod.add_external_symbol("bronze_native_buffer_slot");
        mod.add_external_symbol("bronze_native_buffer_wrap");
        auto brassTypeOf = [](il::Type t) -> brass::Type {
            switch (t) {
                case il::Type::Void: return brass::Type::void_type();
                case il::Type::Bool: return brass::Type::i8();
                case il::Type::I32: return brass::Type::i32();
                case il::Type::F64: return brass::Type::f64();
                case il::Type::Str: return brass::Type::ptr();
                case il::Type::Dynamic: return brass::Type::i64();
            }
            return brass::Type::i64();
        };
        for (size_t i = 0; i < imports.size(); ++i) {
            const il::Function& decl = module.functions[imports[i].functionIndex];
            const bool isClassSlot = imports[i].name.rfind("class ", 0) == 0;
            std::vector<brass::Type> paramTypes;
            for (const auto& p : decl.params) paramTypes.push_back(brassTypeOf(p.type));
            const brass::Type retType = decl.returnType == il::Type::Bool
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
            for (const auto& t : paramTypes) args.push_back(b.add_block_param(entry, t));
            b.position_at_end(entry);
            brass::Value* table = b.build_func_addr(importsSymbol);
            const auto slotOffset = static_cast<int32_t>(8 + i * sizeof(uint64_t));
            if (isClassSlot) {
                b.build_ret(b.build_load(brass::Type::i64(), table, slotOffset));
            } else if (imports[i].bufferReturnKind != UINT32_MAX) {
                brass::Value* slot = b.build_call("bronze_native_buffer_slot", brass::Type::ptr());
                std::vector<brass::Value*> withSlot = args;
                withSlot.push_back(slot);
                brass::Value* callee = b.build_load(brass::Type::ptr(), table, slotOffset);
                b.build_call_indirect(callee, brass::Type::void_type(),
                                      brass::Span<brass::Value* const>(withSlot.data(), withSlot.size()));
                brass::Value* kind =
                    b.build_iconst_i32(static_cast<int32_t>(imports[i].bufferReturnKind));
                b.build_ret(b.build_call("bronze_native_buffer_wrap", brass::Type::i64(), {kind}));
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
        if (!imports.empty()) {
            brass::Function* bind = mod.create_function("__bronze_native_bind", brass::Type::void_type());
            brass::Builder b(mod);
            b.set_function(bind);
            b.append_block("entry");
            brass::Value* table = b.build_func_addr(importsSymbol);
            b.build_call("bronze_native_bind", brass::Type::void_type(), {table});
            b.build_ret_void();
            bind->rebuild_cfg_predecessors();
        }
    }

    brass::Target target = brass::Target::host();
    brass::object::ModuleCompiler compiler(target);
    brass::codegen::SchedOptions schedOpts;
    schedOpts.enable_post_ra = optimize;
    schedOpts.enable_software_pipelining = optimize;
    compiler.set_sched_options(schedOpts);
    compiler.set_enable_trace_layout(optimize);
    compiler.set_enable_mir_opts(optimize);
    timer.mark("thunks");
    brass::object::ObjectFile obj = compiler.compile(*res.module);
    timer.mark("brass compile");

    if (entrySymbol_ != "main") {
        if (auto* sym = obj.find_symbol("main")) {
            sym->name = entrySymbol_;
        }
        for (auto& cfi : obj.functions) {
            if (cfi.name == "main") {
                cfi.name = entrySymbol_;
            }
        }
        for (auto& dt : obj.debug_tables) {
            if (dt.function_name() == "main") {
                dt.set_function_name(entrySymbol_);
            }
        }
        for (auto& sec : obj.sections) {
            for (auto& reloc : sec.relocations) {
                if (reloc.symbol_name == "main") {
                    reloc.symbol_name = entrySymbol_;
                }
            }
        }
    }

    // Everything the runtime reads by name beside brass's code: the rodata
    // tables, the descriptors and pc tables, the data cells and the import
    // table (brass_backend_sections.cpp).
    codegen::SectionInputs sectionInputs{module, uniqueNames, entrySymbol_, hostGlobals_,
                                         globalCacheCount, methodIcSites};
    codegen::emitBronzeSections(obj, target, sectionInputs, timer);

    return obj;
}

bool BrassBackend::emitObject(const il::Module& module, const std::string& outputPath,
                              DiagnosticSink& diags) {
    auto obj = buildObjectFile(module, diags);
    if (!obj) {
        return false;
    }

    brass::Target target = brass::Target::host();
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
        brass::object::MachOWriter writer(*obj);
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
