#include "codegen-brass/brass_backend.h"

#include <brass/brass.hpp>
#include <brass/il_translator/il_translator.hpp>
#include <brass/object/coff_writer.hpp>
#include <brass/object/elf_writer.hpp>
#include <brass/object/object_writer.hpp>
#include <brass/target/target.hpp>

#include <filesystem>

#include "abi/bronze_abi.h"
#include "il/print.h"

namespace bronze {

bool BrassBackend::emitObject(const il::Module& module, const std::string& outputPath,
                              DiagnosticSink& diags) {
    const std::string ilText = il::print(module);

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

    for (auto& sym : obj.symbols) {
        if (sym.section_index != brass::object::SECTION_UNDEF && sym.section_index >= 0) {
            if (sym.name != entrySymbol_ &&
                sym.name != stampSymbol &&
                sym.name != manifestSymbol) {
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
