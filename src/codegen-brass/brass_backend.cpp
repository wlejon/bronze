#include "codegen-brass/brass_backend.h"

#include "abi/bronze_abi.h"
#include "il/print.h"

#include <brass/brass.hpp>
#include <brass/il_translator/il_translator.hpp>
#include <brass/object/coff_writer.hpp>
#include <brass/object/elf_writer.hpp>
#include <brass/object/object_writer.hpp>
#include <brass/target/target.hpp>

#include <filesystem>

namespace bronze {

bool BrassBackend::emitObject(const il::Module& module, const std::string& outputPath,
                              DiagnosticSink& diags) {
    std::vector<std::string> uniqueNames(module.functions.size());
    std::unordered_map<std::string, size_t> nameCounts;
    for (const auto& fn : module.functions) {
        nameCounts[fn.name]++;
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
        if (nameCounts[fn.name] == 1 && !usedNames.count(fn.name)) {
            uniqueNames[i] = fn.name;
            usedNames.insert(fn.name);
        } else {
            std::string uname = fn.name + "$" + std::to_string(i);
            uniqueNames[i] = uname;
            usedNames.insert(uname);
        }
    }

    const std::string ilText = il::print(module, uniqueNames);

    brass::il::TranslatorOptions options;
    options.enable_optimizations = true;
    options.enable_inlining = true;
    options.enable_sroa = true;
    options.enable_gvn = true;
    options.enable_sccp = true;
    options.enable_guard_elim = true;
    options.enable_cfg_simplify = true;
    options.enable_loop_unswitch = true;
    options.enable_jump_threading = true;
    options.enable_trace_layout = true;
    options.enable_pic = sharedRuntime_;
    options.key_constants = module.keyConstants;
    options.entry_symbol = entrySymbol_;
    options.enable_census = !module.censusSites.empty() && !module.censusOutPath.empty();
    options.census_site_count = static_cast<uint32_t>(module.censusSites.size());
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
    brass::il::TranslationResult res = brass::il::translate_bronze_il(ilText, options, &reporter);
    if (!res.success || !res.module || reporter.has_errors()) {
        std::string msg = reporter.has_errors() ? reporter.format_all() : res.error_message;
        if (msg.empty()) {
            msg = "Failed to translate Bronze IL to Brass MIR";
        }
        diags.error(Span{}, msg);
        return false;
    }

    if (entrySymbol_ != "main") {
        if (auto* fn = res.module->get_function("main")) {
            fn->set_name(res.module->string_pool().intern(entrySymbol_));
        }
    }

    brass::Target target = brass::Target::host();
    brass::object::ModuleCompiler compiler(target);
    brass::object::ObjectFile obj = compiler.compile(*res.module);

    if (entrySymbol_ != "main") {
        if (auto* sym = obj.find_symbol("main")) {
            sym->name = entrySymbol_;
        }
        for (auto& cfi : obj.functions) {
            if (cfi.name == "main") {
                cfi.name = entrySymbol_;
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

        if (auto* sym = obj.find_symbol("__bronze_census_out_path")) {
            sym->section_index = obj.get_section_index(roSecName);
            sym->value = outPathOffset;
            sym->size = outPathSize;
            sym->binding = brass::object::SymbolBinding::Local;
            sym->type = brass::object::SymbolType::Object;
        } else {
            brass::object::ObjectSymbol outPathSym;
            outPathSym.name = "__bronze_census_out_path";
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

        if (auto* sym = obj.find_symbol("__bronze_census_sites")) {
            sym->section_index = obj.get_section_index(roSecName);
            sym->value = sitesOffset;
            sym->size = sitesSize;
            sym->binding = brass::object::SymbolBinding::Local;
            sym->type = brass::object::SymbolType::Object;
        } else {
            brass::object::ObjectSymbol sitesSym;
            sitesSym.name = "__bronze_census_sites";
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

        std::string textSymName = "__bronze_source_text_" + std::to_string(file);
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

        std::string entriesSymName = "__bronze_source_entries_" + std::to_string(file);
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

    if (auto* sym = obj.find_symbol("__bronze_module_env")) {
        sym->section_index = obj.get_section_index(dataSecName);
        sym->value = envOffset;
        sym->size = 8;
        sym->binding = brass::object::SymbolBinding::Local;
        sym->type = brass::object::SymbolType::Object;
    } else {
        brass::object::ObjectSymbol envSym;
        envSym.name = "__bronze_module_env";
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

    if (auto* sym = obj.find_symbol("__bronze_key_map")) {
        sym->section_index = obj.get_section_index(dataSecName);
        sym->value = keyMapOffset;
        sym->size = keyMapBytes;
        sym->binding = brass::object::SymbolBinding::Local;
        sym->type = brass::object::SymbolType::Object;
    } else {
        brass::object::ObjectSymbol kmSym;
        kmSym.name = "__bronze_key_map";
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

    if (auto* sym = obj.find_symbol("__bronze_template_cells")) {
        sym->section_index = obj.get_section_index(dataSecName);
        sym->value = tplOffset;
        sym->size = tplBytes;
        sym->binding = brass::object::SymbolBinding::Local;
        sym->type = brass::object::SymbolType::Object;
    } else {
        brass::object::ObjectSymbol tplSym;
        tplSym.name = "__bronze_template_cells";
        tplSym.section_index = obj.get_section_index(dataSecName);
        tplSym.value = tplOffset;
        tplSym.size = tplBytes;
        tplSym.binding = brass::object::SymbolBinding::Local;
        tplSym.type = brass::object::SymbolType::Object;
        obj.add_symbol(std::move(tplSym));
    }

    for (auto& sym : obj.symbols) {
        if (sym.section_index != brass::object::SECTION_UNDEF && sym.section_index >= 0) {
            if (sym.name != entrySymbol_ &&
                sym.name != stampSymbol &&
                sym.name != manifestSymbol &&
                sym.name != keySymbol) {
                sym.binding = brass::object::SymbolBinding::Local;
            }
        }
    }

    std::error_code ec;
    std::filesystem::path outPath(outputPath);
    if (outPath.has_parent_path()) {
        std::filesystem::create_directories(outPath.parent_path(), ec);
    }

    bool writeSuccess = false;
    if (target.is_windows()) {
        brass::object::CoffWriter writer(obj);
        writeSuccess = writer.write_to_file(outputPath);
    } else {
        brass::object::ElfWriter writer(obj);
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
