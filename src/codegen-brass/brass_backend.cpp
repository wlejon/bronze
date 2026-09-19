#include "codegen-brass/brass_backend.h"
#include "codegen-brass/il_to_brass_ast.h"

#include "abi/bronze_abi.h"
#include "support/source.h"
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
    // site, the verifier bounded each number, and `__bronze_ic_table` below
    // is laid out to exactly this count, so brass may address a site as
    // `table + index * BRONZE_ABI_IC_SITE_SIZE` without a check.
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
        if (entrySymbol_.empty() || entrySymbol_ == "main" || entrySymbol_ == "bronze_main") {
            return base;
        }
        return base + "_" + entrySymbol_;
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

    const std::string keySymbol = (entrySymbol_ == "bronze_main")
                                      ? "bronze_main_key_constants"
                                      : (entrySymbol_ + "_key_constants");
    if (keySymbol != "bronze_main_key_constants") {
        if (auto* sym = obj.find_symbol("bronze_main_key_constants")) {
            sym->name = keySymbol;
        }
        for (auto& sec : obj.sections) {
            for (auto& reloc : sec.relocations) {
                if (reloc.symbol_name == "bronze_main_key_constants") {
                    reloc.symbol_name = keySymbol;
                }
            }
        }
    }

    std::string roSecName = target.is_windows() ? ".rdata" : (target.is_macos() ? "__const" : ".rodata");
    brass::object::Section& roSec = obj.get_or_create_section(
        roSecName,
        brass::object::SectionKind::RoData,
        brass::object::SectionFlags::Read | brass::object::SectionFlags::Alloc,
        16
    );

    roSec.align_to(4);
    const size_t stampOffset = roSec.data.size();
    roSec.emit32(BRONZE_ABI_FINGERPRINT);

    const std::string stampSymbol = (entrySymbol_ == "bronze_main")
                                        ? "bronze_object_abi_fingerprint"
                                        : (entrySymbol_ + "_abi_fingerprint");
    brass::object::ObjectSymbol stampSym;
    stampSym.name = stampSymbol;
    stampSym.section_index = obj.get_section_index(roSecName);
    stampSym.value = stampOffset;
    stampSym.size = 4;
    stampSym.binding = brass::object::SymbolBinding::Global;
    stampSym.type = brass::object::SymbolType::Object;
    obj.add_symbol(std::move(stampSym));

    roSec.align_to(4);
    const size_t manifestOffset = roSec.data.size();
    const uint32_t count = static_cast<uint32_t>(hostGlobals_.size());
    roSec.emit32(count);
    for (const std::string& name : hostGlobals_) {
        roSec.emit_bytes(reinterpret_cast<const uint8_t*>(name.data()), name.size());
        roSec.emit8(0);
    }
    const size_t manifestSize = roSec.data.size() - manifestOffset;

    const std::string manifestSymbol = entrySymbol_ + "_host_globals";
    brass::object::ObjectSymbol manifestSym;
    manifestSym.name = manifestSymbol;
    manifestSym.section_index = obj.get_section_index(roSecName);
    manifestSym.value = manifestOffset;
    manifestSym.size = manifestSize;
    manifestSym.binding = brass::object::SymbolBinding::Global;
    manifestSym.type = brass::object::SymbolType::Object;
    obj.add_symbol(std::move(manifestSym));

    roSec.align_to(4);
    const size_t keyOffset = roSec.data.size();
    const uint32_t keyCount = static_cast<uint32_t>(module.keyConstants.size());
    roSec.emit32(keyCount);
    for (const std::string& key : module.keyConstants) {
        roSec.emit32(static_cast<uint32_t>(key.size()));
        roSec.emit_bytes(reinterpret_cast<const uint8_t*>(key.data()), key.size());
        roSec.emit8(0);
    }
    const size_t keySize = roSec.data.size() - keyOffset;

    brass::object::ObjectSymbol keySym;
    keySym.name = keySymbol;
    keySym.section_index = obj.get_section_index(roSecName);
    keySym.value = keyOffset;
    keySym.size = keySize;
    keySym.binding = brass::object::SymbolBinding::Global;
    keySym.type = brass::object::SymbolType::Object;
    obj.add_symbol(std::move(keySym));

    if (!module.censusSites.empty() && !module.censusOutPath.empty()) {
        roSec.align_to(1);
        const size_t outPathOffset = roSec.data.size();
        roSec.emit_bytes(reinterpret_cast<const uint8_t*>(module.censusOutPath.data()),
                         module.censusOutPath.size());
        roSec.emit8(0);
        const size_t outPathSize = roSec.data.size() - outPathOffset;

        if (auto* sym = obj.find_symbol(moduleSym("__bronze_census_out_path"))) {
            sym->section_index = obj.get_section_index(roSecName);
            sym->value = outPathOffset;
            sym->size = outPathSize;
            sym->binding = brass::object::SymbolBinding::Local;
            sym->type = brass::object::SymbolType::Object;
        } else {
            brass::object::ObjectSymbol outPathSym;
            outPathSym.name = moduleSym("__bronze_census_out_path");
            outPathSym.section_index = obj.get_section_index(roSecName);
            outPathSym.value = outPathOffset;
            outPathSym.size = outPathSize;
            outPathSym.binding = brass::object::SymbolBinding::Local;
            outPathSym.type = brass::object::SymbolType::Object;
            obj.add_symbol(std::move(outPathSym));
        }

        roSec.align_to(4);
        const size_t sitesOffset = roSec.data.size();
        for (const auto& site : module.censusSites) {
            roSec.emit32(site.keyIndex);
            roSec.emit32(site.info);
        }
        const size_t sitesSize = roSec.data.size() - sitesOffset;

        if (auto* sym = obj.find_symbol(moduleSym("__bronze_census_sites"))) {
            sym->section_index = obj.get_section_index(roSecName);
            sym->value = sitesOffset;
            sym->size = sitesSize;
            sym->binding = brass::object::SymbolBinding::Local;
            sym->type = brass::object::SymbolType::Object;
        } else {
            brass::object::ObjectSymbol sitesSym;
            sitesSym.name = moduleSym("__bronze_census_sites");
            sitesSym.section_index = obj.get_section_index(roSecName);
            sitesSym.value = sitesOffset;
            sitesSym.size = sitesSize;
            sitesSym.binding = brass::object::SymbolBinding::Local;
            sitesSym.type = brass::object::SymbolType::Object;
            obj.add_symbol(std::move(sitesSym));
        }
    }

    // The method-call site numbers, as the u64 array the module's entry hands
    // `bronze_register_method_ic_cells` with the IC table (bronze_abi.h). Only
    // emitted when there is a site to register; brass emits the call under the
    // same condition.
    if (!methodIcSites.empty()) {
        roSec.align_to(8);
        const size_t sitesOffset = roSec.data.size();
        for (uint32_t site : methodIcSites) {
            roSec.emit64(static_cast<uint64_t>(site));
        }
        const size_t sitesSize = roSec.data.size() - sitesOffset;
        const std::string sitesSymName = moduleSym("__bronze_method_ic_sites");
        if (auto* sym = obj.find_symbol(sitesSymName)) {
            sym->section_index = obj.get_section_index(roSecName);
            sym->value = sitesOffset;
            sym->size = sitesSize;
            sym->binding = brass::object::SymbolBinding::Local;
            sym->type = brass::object::SymbolType::Object;
        } else {
            brass::object::ObjectSymbol sitesSym;
            sitesSym.name = sitesSymName;
            sitesSym.section_index = obj.get_section_index(roSecName);
            sitesSym.value = sitesOffset;
            sitesSym.size = sitesSize;
            sitesSym.binding = brass::object::SymbolBinding::Local;
            sitesSym.type = brass::object::SymbolType::Object;
            obj.add_symbol(std::move(sitesSym));
        }
    }

    for (uint16_t file = 0; file < module.sourceTexts.size(); ++file) {
        uint32_t entry_count = 0;
        for (size_t i = 0; i < module.functions.size(); ++i) {
            const auto& fn = module.functions[i];
            if (fn.sourceFile == file && fn.sourceEnd > fn.sourceBegin && uniqueNames[i] != "main") {
                entry_count++;
            }
        }
        if (entry_count == 0) continue;

        roSec.align_to(1);
        const size_t textOffset = roSec.data.size();
        const std::string& text = module.sourceTexts[file];
        roSec.emit_bytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());
        roSec.emit8(0);
        const size_t textSize = text.size() + 1;

        std::string textSymName = moduleSym("__bronze_source_text_" + std::to_string(file));
        if (auto* sym = obj.find_symbol(textSymName)) {
            sym->section_index = obj.get_section_index(roSecName);
            sym->value = textOffset;
            sym->size = textSize;
            sym->binding = brass::object::SymbolBinding::Local;
            sym->type = brass::object::SymbolType::Object;
        } else {
            brass::object::ObjectSymbol textSym;
            textSym.name = textSymName;
            textSym.section_index = obj.get_section_index(roSecName);
            textSym.value = textOffset;
            textSym.size = textSize;
            textSym.binding = brass::object::SymbolBinding::Local;
            textSym.type = brass::object::SymbolType::Object;
            obj.add_symbol(std::move(textSym));
        }

        roSec.align_to(8);
        const size_t entriesOffset = roSec.data.size();
        for (size_t i = 0; i < module.functions.size(); ++i) {
            const auto& fn = module.functions[i];
            if (fn.sourceFile != file || fn.sourceEnd <= fn.sourceBegin || uniqueNames[i] == "main") continue;
            brass::object::ObjectRelocation reloc;
            reloc.offset = roSec.data.size();
            reloc.symbol_name = "__wrapper_" + uniqueNames[i];
            reloc.kind = brass::object::RelocKind::Abs64;
            reloc.addend = 0;
            roSec.relocations.push_back(reloc);
            roSec.emit64(0);
            uint64_t span = (static_cast<uint64_t>(fn.sourceBegin) << 32) |
                            static_cast<uint64_t>(fn.sourceEnd - fn.sourceBegin);
            roSec.emit64(span);
        }
        const size_t entriesSize = entry_count * 16;

        std::string entriesSymName = moduleSym("__bronze_source_entries_" + std::to_string(file));
        if (auto* sym = obj.find_symbol(entriesSymName)) {
            sym->section_index = obj.get_section_index(roSecName);
            sym->value = entriesOffset;
            sym->size = entriesSize;
            sym->binding = brass::object::SymbolBinding::Local;
            sym->type = brass::object::SymbolType::Object;
        } else {
            brass::object::ObjectSymbol entriesSym;
            entriesSym.name = entriesSymName;
            entriesSym.section_index = obj.get_section_index(roSecName);
            entriesSym.value = entriesOffset;
            entriesSym.size = entriesSize;
            entriesSym.binding = brass::object::SymbolBinding::Local;
            entriesSym.type = brass::object::SymbolType::Object;
            obj.add_symbol(std::move(entriesSym));
        }
    }

    auto emitStringSym = [&](const std::string& symName, const std::string& str) {
        roSec.align_to(1);
        const size_t strOffset = roSec.data.size();
        roSec.emit_bytes(reinterpret_cast<const uint8_t*>(str.data()), str.size());
        roSec.emit8(0);
        const size_t strSize = str.size() + 1;

        if (auto* sym = obj.find_symbol(symName)) {
            sym->section_index = obj.get_section_index(roSecName);
            sym->value = strOffset;
            sym->size = strSize;
            sym->binding = brass::object::SymbolBinding::Local;
            sym->type = brass::object::SymbolType::Object;
        } else {
            brass::object::ObjectSymbol s;
            s.name = symName;
            s.section_index = obj.get_section_index(roSecName);
            s.value = strOffset;
            s.size = strSize;
            s.binding = brass::object::SymbolBinding::Local;
            s.type = brass::object::SymbolType::Object;
            obj.add_symbol(std::move(s));
        }
    };

    emitStringSym(moduleSym("__bronze_file_str_empty"), "");
    for (size_t f = 0; f < module.sourceFiles.size(); ++f) {
        emitStringSym(moduleSym("__bronze_file_str_" + std::to_string(f)), module.sourceFiles[f]);
    }
    timer.mark("ro tables");

    // One line index per file for the descriptors' (line, column): the
    // per-function scan from byte 0 it replaces was 4.7 s of a pixi compile.
    std::vector<LineTable> lineTables;
    lineTables.reserve(module.sourceTexts.size());
    for (const std::string& text : module.sourceTexts) lineTables.emplace_back(text);

    for (size_t i = 0; i < module.functions.size(); ++i) {
        const auto& fn = module.functions[i];
        if (fn.blocks.empty()) continue;

        std::string nameStr;
        if (!fn.displayName.empty()) {
            nameStr = fn.displayName;
        } else if (uniqueNames[i] == "main" || fn.name == "main") {
            nameStr = "";
        } else if (fn.name.rfind("__anon_fn", 0) == 0) {
            nameStr = "<anonymous>";
        } else {
            nameStr = fn.name;
        }

        std::string nameSymName = moduleSym("__bronze_fn_name_" + uniqueNames[i]);
        emitStringSym(nameSymName, nameStr);

        std::string fileSymName;
        if (fn.sourceFile < module.sourceFiles.size()) {
            fileSymName = moduleSym("__bronze_file_str_" + std::to_string(fn.sourceFile));
        } else {
            fileSymName = moduleSym("__bronze_file_str_empty");
        }

        uint32_t line = 1, col = 1;
        if (fn.sourceFile < lineTables.size()) {
            const SourceBuffer::LineCol lc = lineTables[fn.sourceFile].lineCol(fn.sourceBegin);
            line = lc.line;
            col = lc.column;
        }

        roSec.align_to(8);
        const size_t descOffset = roSec.data.size();

        brass::object::ObjectRelocation nameReloc;
        nameReloc.offset = roSec.data.size();
        nameReloc.symbol_name = nameSymName;
        nameReloc.kind = brass::object::RelocKind::Abs64;
        nameReloc.addend = 0;
        roSec.relocations.push_back(nameReloc);
        roSec.emit64(0);

        brass::object::ObjectRelocation fileReloc;
        fileReloc.offset = roSec.data.size();
        fileReloc.symbol_name = fileSymName;
        fileReloc.kind = brass::object::RelocKind::Abs64;
        fileReloc.addend = 0;
        roSec.relocations.push_back(fileReloc);
        roSec.emit64(0);

        roSec.emit32(line);
        roSec.emit32(col);
        roSec.emit32(fn.descFlags);
        roSec.emit32(0);

        brass::object::ObjectRelocation codeReloc;
        codeReloc.offset = roSec.data.size();
        codeReloc.symbol_name = (uniqueNames[i] == "main")
            ? (entrySymbol_.empty() ? "main" : entrySymbol_)
            : ("__wrapper_" + uniqueNames[i]);
        codeReloc.kind = brass::object::RelocKind::Abs64;
        codeReloc.addend = 0;
        roSec.relocations.push_back(codeReloc);
        roSec.emit64(0);

        std::string descSymName = moduleSym("__bronze_fn_desc_" + uniqueNames[i]);
        if (auto* sym = obj.find_symbol(descSymName)) {
            sym->section_index = obj.get_section_index(roSecName);
            sym->value = descOffset;
            sym->size = 40;
            sym->binding = brass::object::SymbolBinding::Global;
            sym->type = brass::object::SymbolType::Object;
        } else {
            brass::object::ObjectSymbol descSym;
            descSym.name = descSymName;
            descSym.section_index = obj.get_section_index(roSecName);
            descSym.value = descOffset;
            descSym.size = 40;
            descSym.binding = brass::object::SymbolBinding::Global;
            descSym.type = brass::object::SymbolType::Object;
            obj.add_symbol(std::move(descSym));
        }
    }

    auto emitRoReloc = [&](const std::string& sym) {
        if (!sym.empty()) {
            roSec.relocations.push_back({roSec.data.size(), brass::object::RelocKind::Abs64, sym, 0});
        }
        roSec.emit64(0);
    };
    auto addRoSym = [&](const std::string& name, size_t off, size_t sz, brass::object::SymbolBinding bind) {
        if (auto* s = obj.find_symbol(name)) {
            s->section_index = obj.get_section_index(roSecName);
            s->value = off; s->size = sz; s->binding = bind; s->type = brass::object::SymbolType::Object;
        } else {
            obj.add_symbol({name, obj.get_section_index(roSecName), off, sz, bind, brass::object::SymbolType::Object});
        }
    };

    timer.mark("descriptors");
    std::unordered_map<std::string, std::string> fnToDesc;
    for (size_t i = 0; i < module.functions.size(); ++i) {
        if (module.functions[i].blocks.empty()) continue;
        std::string fnName = (uniqueNames[i] == "main")
            ? (entrySymbol_.empty() ? "main" : entrySymbol_) : uniqueNames[i];
        fnToDesc[fnName] = moduleSym("__bronze_fn_desc_" + uniqueNames[i]);
    }

    std::unordered_map<std::string, const brass::FunctionDebugTable*> debugTableMap;
    for (const auto& dt : obj.debug_tables) debugTableMap[dt.function_name()] = &dt;

    std::unordered_map<std::string, std::string> fnToPcTable;
    for (const auto& cfi : obj.functions) {
        auto itDt = debugTableMap.find(cfi.name);
        if (itDt != debugTableMap.end() && !itDt->second->line_entries().empty()) {
            const auto& entries = itDt->second->line_entries();
            roSec.align_to(4);
            const size_t pcOff = roSec.data.size();
            for (const auto& le : entries) {
                roSec.emit32(le.code_offset); roSec.emit32(le.loc.line); roSec.emit32(le.loc.column);
            }
            std::string pcSym = moduleSym("__bronze_pc_table_" + cfi.name);
            fnToPcTable[cfi.name] = pcSym;
            addRoSym(pcSym, pcOff, entries.size() * sizeof(bronze_pc_entry), brass::object::SymbolBinding::Local);
        }
    }

    timer.mark("pc tables");
    const std::string codeRangesSymbol = (entrySymbol_ == "bronze_main")
        ? "bronze_object_code_ranges" : (entrySymbol_ + "_code_ranges");
    const std::string codeRangeCountSymbol = (entrySymbol_ == "bronze_main")
        ? "bronze_object_code_range_count" : (entrySymbol_ + "_code_range_count");

    roSec.align_to(4);
    const size_t countOff = roSec.data.size();
    roSec.emit32(static_cast<uint32_t>(obj.functions.size()));
    addRoSym(codeRangeCountSymbol, countOff, sizeof(uint32_t), brass::object::SymbolBinding::Global);

    roSec.align_to(8);
    const size_t rangesOff = roSec.data.size();
    for (const auto& cfi : obj.functions) {
        emitRoReloc(cfi.name);
        roSec.emit32(static_cast<uint32_t>(cfi.text_size));
        auto itDt = debugTableMap.find(cfi.name);
        roSec.emit32(itDt != debugTableMap.end() ? static_cast<uint32_t>(itDt->second->line_entries().size()) : 0);
        auto itDesc = fnToDesc.find(cfi.name);
        emitRoReloc(itDesc != fnToDesc.end() ? itDesc->second : "");
        auto itPc = fnToPcTable.find(cfi.name);
        emitRoReloc(itPc != fnToPcTable.end() ? itPc->second : "");
    }
    addRoSym(codeRangesSymbol, rangesOff, obj.functions.size() * sizeof(bronze_code_range), brass::object::SymbolBinding::Global);
    timer.mark("code ranges");

    std::string dataSecName = target.is_windows() ? ".data" : ".data";
    brass::object::Section& dataSec = obj.get_or_create_section(
        dataSecName,
        brass::object::SectionKind::Data,
        brass::object::SectionFlags::Read | brass::object::SectionFlags::Write | brass::object::SectionFlags::Alloc,
        8
    );
    dataSec.align_to(8);
    const size_t envOffset = dataSec.data.size();
    dataSec.emit64(BRONZE_ABI_UNDEFINED_BITS);

    if (auto* sym = obj.find_symbol(moduleSym("__bronze_module_env"))) {
        sym->section_index = obj.get_section_index(dataSecName);
        sym->value = envOffset;
        sym->size = 8;
        sym->binding = brass::object::SymbolBinding::Local;
        sym->type = brass::object::SymbolType::Object;
    } else {
        brass::object::ObjectSymbol envSym;
        envSym.name = moduleSym("__bronze_module_env");
        envSym.section_index = obj.get_section_index(dataSecName);
        envSym.value = envOffset;
        envSym.size = 8;
        envSym.binding = brass::object::SymbolBinding::Local;
        envSym.type = brass::object::SymbolType::Object;
        obj.add_symbol(std::move(envSym));
    }

    dataSec.align_to(4);
    const size_t keyMapOffset = dataSec.data.size();
    const size_t keyMapBytes = std::max<size_t>(static_cast<size_t>(keyCount) * sizeof(uint32_t), sizeof(uint32_t));
    dataSec.data.resize(dataSec.data.size() + keyMapBytes, 0);

    if (auto* sym = obj.find_symbol(moduleSym("__bronze_key_map"))) {
        sym->section_index = obj.get_section_index(dataSecName);
        sym->value = keyMapOffset;
        sym->size = keyMapBytes;
        sym->binding = brass::object::SymbolBinding::Local;
        sym->type = brass::object::SymbolType::Object;
    } else {
        brass::object::ObjectSymbol kmSym;
        kmSym.name = moduleSym("__bronze_key_map");
        kmSym.section_index = obj.get_section_index(dataSecName);
        kmSym.value = keyMapOffset;
        kmSym.size = keyMapBytes;
        kmSym.binding = brass::object::SymbolBinding::Local;
        kmSym.type = brass::object::SymbolType::Object;
        obj.add_symbol(std::move(kmSym));
    }

    dataSec.align_to(8);
    const size_t tplOffset = dataSec.data.size();
    const size_t tplCount = 1024;
    const size_t tplBytes = tplCount * sizeof(uint64_t);
    const size_t curTplSize = dataSec.data.size();
    dataSec.data.resize(curTplSize + tplBytes);
    for (size_t i = 0; i < tplCount; ++i) {
        *reinterpret_cast<uint64_t*>(&dataSec.data[curTplSize + i * sizeof(uint64_t)]) = BRONZE_ABI_UNDEFINED_BITS;
    }

    if (auto* sym = obj.find_symbol(moduleSym("__bronze_template_cells"))) {
        sym->section_index = obj.get_section_index(dataSecName);
        sym->value = tplOffset;
        sym->size = tplBytes;
        sym->binding = brass::object::SymbolBinding::Local;
        sym->type = brass::object::SymbolType::Object;
    } else {
        brass::object::ObjectSymbol tplSym;
        tplSym.name = moduleSym("__bronze_template_cells");
        tplSym.section_index = obj.get_section_index(dataSecName);
        tplSym.value = tplOffset;
        tplSym.size = tplBytes;
        tplSym.binding = brass::object::SymbolBinding::Local;
        tplSym.type = brass::object::SymbolType::Object;
        obj.add_symbol(std::move(tplSym));
    }

    // The host-global read cache the thunks above index: one hole per slot.
    dataSec.align_to(8);
    const size_t cacheOffset = dataSec.data.size();
    const size_t cacheCells = std::max<size_t>(globalCacheCount, 1);
    const size_t cacheBytes = cacheCells * sizeof(uint64_t);
    dataSec.data.resize(cacheOffset + cacheBytes);
    for (size_t i = 0; i < cacheCells; ++i) {
        *reinterpret_cast<uint64_t*>(&dataSec.data[cacheOffset + i * sizeof(uint64_t)]) = BRONZE_ABI_NO_EXCEPTION_BITS;
    }
    if (auto* sym = obj.find_symbol(globalCacheSym)) {
        sym->section_index = obj.get_section_index(dataSecName);
        sym->value = cacheOffset;
        sym->size = cacheBytes;
        sym->binding = brass::object::SymbolBinding::Local;
        sym->type = brass::object::SymbolType::Object;
    } else {
        brass::object::ObjectSymbol cacheSymbol;
        cacheSymbol.name = globalCacheSym;
        cacheSymbol.section_index = obj.get_section_index(dataSecName);
        cacheSymbol.value = cacheOffset;
        cacheSymbol.size = cacheBytes;
        cacheSymbol.binding = brass::object::SymbolBinding::Local;
        cacheSymbol.type = brass::object::SymbolType::Object;
        obj.add_symbol(std::move(cacheSymbol));
    }

    // The inline-cache table (bronze_abi.h, "the inline property cache
    // contract"): one BRONZE_ABI_IC_SITE_SIZE-byte site per property or
    // method site lowering numbered, every word zero — the empty site. Brass
    // addresses a site as `table + icIndex * BRONZE_ABI_IC_SITE_SIZE` and
    // passes that pointer to the helpers, which fill it in place; the
    // method-call sites' env words were registered above as value cells. At
    // least one site is laid out so the symbol always has a size.
    dataSec.align_to(8);
    const size_t icTableOffset = dataSec.data.size();
    const size_t icSites = std::max<size_t>(module.icSiteCount, 1);
    const size_t icTableBytes = icSites * BRONZE_ABI_IC_SITE_SIZE;
    dataSec.data.resize(icTableOffset + icTableBytes, 0);
    const std::string icTableSym = moduleSym("__bronze_ic_table");
    if (auto* sym = obj.find_symbol(icTableSym)) {
        sym->section_index = obj.get_section_index(dataSecName);
        sym->value = icTableOffset;
        sym->size = icTableBytes;
        sym->binding = brass::object::SymbolBinding::Local;
        sym->type = brass::object::SymbolType::Object;
    } else {
        brass::object::ObjectSymbol icSym;
        icSym.name = icTableSym;
        icSym.section_index = obj.get_section_index(dataSecName);
        icSym.value = icTableOffset;
        icSym.size = icTableBytes;
        icSym.binding = brass::object::SymbolBinding::Local;
        icSym.type = brass::object::SymbolType::Object;
        obj.add_symbol(std::move(icSym));
    }

    // The native import table: { u32 count; u32 namesOffset; u64 slots[];
    // names... }. Every function slot starts as the address of
    // bronze_native_unbound — an absolute relocation the loader resolves — so
    // a call through a slot the bind never reached traps by name instead of
    // jumping through zero; a class slot starts as 0 (the helpers that read
    // one are fatal on null). Exported: the loader resolves it beside the
    // entry, and defined for a module with no imports too, with count 0.
    dataSec.align_to(8);
    const size_t importsOffset = dataSec.data.size();
    dataSec.emit32(static_cast<uint32_t>(imports.size()));
    dataSec.emit32(static_cast<uint32_t>(8 + imports.size() * sizeof(uint64_t)));
    for (size_t i = 0; i < imports.size(); ++i) {
        const bool isClassSlot = imports[i].name.rfind("class ", 0) == 0;
        if (!isClassSlot) {
            brass::object::ObjectRelocation reloc;
            reloc.offset = dataSec.data.size();
            reloc.kind = brass::object::RelocKind::Abs64;
            reloc.symbol_name = "bronze_native_unbound";
            reloc.addend = 0;
            dataSec.relocations.push_back(std::move(reloc));
        }
        dataSec.emit64(0);
    }
    for (const auto& imp : imports) {
        dataSec.emit_bytes(reinterpret_cast<const uint8_t*>(imp.name.data()), imp.name.size());
        dataSec.emit8(0);
        dataSec.emit_bytes(reinterpret_cast<const uint8_t*>(imp.signature.data()), imp.signature.size());
        dataSec.emit8(0);
    }
    const size_t importsSize = dataSec.data.size() - importsOffset;
    if (auto* sym = obj.find_symbol(importsSymbol)) {
        sym->section_index = obj.get_section_index(dataSecName);
        sym->value = importsOffset;
        sym->size = importsSize;
        sym->binding = brass::object::SymbolBinding::Global;
        sym->type = brass::object::SymbolType::Object;
    } else {
        brass::object::ObjectSymbol importsSym;
        importsSym.name = importsSymbol;
        importsSym.section_index = obj.get_section_index(dataSecName);
        importsSym.value = importsOffset;
        importsSym.size = importsSize;
        importsSym.binding = brass::object::SymbolBinding::Global;
        importsSym.type = brass::object::SymbolType::Object;
        obj.add_symbol(std::move(importsSym));
    }
    if (!obj.find_symbol("bronze_native_unbound")) {
        // The relocation target, declared undefined so the writers emit it
        // as an import rather than refusing an unknown name.
        brass::object::ObjectSymbol unbound;
        unbound.name = "bronze_native_unbound";
        unbound.section_index = brass::object::SECTION_UNDEF;
        unbound.binding = brass::object::SymbolBinding::Global;
        unbound.type = brass::object::SymbolType::Function;
        obj.add_symbol(std::move(unbound));
    }

    for (auto& sym : obj.symbols) {
        if (sym.section_index != brass::object::SECTION_UNDEF && sym.section_index >= 0) {
            if (sym.name != entrySymbol_ &&
                sym.name != stampSymbol &&
                sym.name != manifestSymbol &&
                sym.name != keySymbol &&
                sym.name != importsSymbol &&
                sym.name != codeRangesSymbol &&
                sym.name != codeRangeCountSymbol) {
                sym.binding = brass::object::SymbolBinding::Local;
            }
        }
    }
    timer.mark("data section");
    if (support::timingsEnabled()) {
        size_t lineEntries = 0;
        for (const auto& dt : obj.debug_tables) lineEntries += dt.line_entries().size();
        size_t relocs = 0;
        for (const auto& sec : obj.sections) relocs += sec.relocations.size();
        std::fprintf(stderr, "    functions %zu, symbols %zu, relocations %zu, line entries %zu\n",
                     obj.functions.size(), obj.symbols.size(), relocs, lineEntries);
    }

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
