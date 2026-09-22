#include "codegen-brass/brass_backend_sections.h"

#include "abi/bronze_abi.h"
#include "support/source.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bronze::codegen {

std::string moduleSymbolName(const std::string& entrySymbol, const std::string& base) {
    if (entrySymbol.empty() || entrySymbol == "main" || entrySymbol == "bronze_main") {
        return base;
    }
    return base + "_" + entrySymbol;
}

namespace {

using brass::object::ObjectFile;
using brass::object::ObjectRelocation;
using brass::object::ObjectSymbol;
using brass::object::RelocKind;
using brass::object::Section;
using brass::object::SymbolBinding;
using brass::object::SymbolType;

// Defines `name` as an object symbol at `off` in section `sectionIndex`.
void defineSymbol(ObjectFile& obj, std::string name, int32_t sectionIndex,
                  size_t off, size_t size, SymbolBinding binding) {
    obj.add_symbol({std::move(name), sectionIndex, off, size, binding, SymbolType::Object});
}

// An absolute 64-bit slot naming `sym`, or an untouched zero slot for "".
void emitPointer(Section& sec, const std::string& sym, int64_t addend = 0) {
    if (!sym.empty()) {
        sec.relocations.push_back({sec.data.size(), RelocKind::Abs64, sym, addend});
    }
    sec.emit64(0);
}

void emitStringSymbol(ObjectFile& obj, Section& sec, int32_t secIdx,
                      std::string symName, std::string_view str,
                      SymbolBinding binding = SymbolBinding::Local) {
    sec.align_to(1);
    const size_t strOffset = sec.data.size();
    sec.emit_bytes(reinterpret_cast<const uint8_t*>(str.data()), str.size());
    sec.emit8(0);
    defineSymbol(obj, std::move(symName), secIdx, strOffset, str.size() + 1, binding);
}

void emitAbiStamp(ObjectFile& obj, Section& roSec, int32_t roIdx, const std::string& entrySymbol) {
    roSec.align_to(4);
    const size_t stampOffset = roSec.data.size();
    roSec.emit32(BRONZE_ABI_FINGERPRINT);
    const std::string stampSymbol = (entrySymbol == "bronze_main")
                                        ? "bronze_object_abi_fingerprint"
                                        : (entrySymbol + "_abi_fingerprint");
    defineSymbol(obj, stampSymbol, roIdx, stampOffset, 4, SymbolBinding::Global);
}

void emitHostGlobalsManifest(ObjectFile& obj, Section& roSec, int32_t roIdx,
                             const std::string& entrySymbol,
                             const std::vector<std::string>& hostGlobals) {
    roSec.align_to(4);
    const size_t manifestOffset = roSec.data.size();
    roSec.emit32(static_cast<uint32_t>(hostGlobals.size()));
    for (const std::string& name : hostGlobals) {
        roSec.emit_bytes(reinterpret_cast<const uint8_t*>(name.data()), name.size());
        roSec.emit8(0);
    }
    const std::string manifestSymbol = entrySymbol + "_host_globals";
    defineSymbol(obj, manifestSymbol, roIdx, manifestOffset, roSec.data.size() - manifestOffset,
                 SymbolBinding::Global);
}

void emitKeyConstants(ObjectFile& obj, Section& roSec, int32_t roIdx,
                      const std::string& keySymbol,
                      const std::vector<std::string>& keyConstants) {
    roSec.align_to(4);
    const size_t keyOffset = roSec.data.size();
    roSec.emit32(static_cast<uint32_t>(keyConstants.size()));
    for (const std::string& key : keyConstants) {
        roSec.emit32(static_cast<uint32_t>(key.size()));
        roSec.emit_bytes(reinterpret_cast<const uint8_t*>(key.data()), key.size());
        roSec.emit8(0);
    }
    defineSymbol(obj, keySymbol, roIdx, keyOffset, roSec.data.size() - keyOffset, SymbolBinding::Global);
}

void emitCensusData(ObjectFile& obj, Section& roSec, int32_t roIdx,
                    const std::string& entrySymbol, const il::Module& module) {
    if (module.censusSites.empty() || module.censusOutPath.empty()) return;

    emitStringSymbol(obj, roSec, roIdx, moduleSymbolName(entrySymbol, "__bronze_census_out_path"),
                     module.censusOutPath, SymbolBinding::Local);

    roSec.align_to(4);
    const size_t sitesOffset = roSec.data.size();
    for (const auto& site : module.censusSites) {
        roSec.emit32(site.keyIndex);
        roSec.emit32(site.info);
    }
    defineSymbol(obj, moduleSymbolName(entrySymbol, "__bronze_census_sites"), roIdx, sitesOffset,
                 roSec.data.size() - sitesOffset, SymbolBinding::Local);
}

void emitMethodIcSites(ObjectFile& obj, Section& roSec, int32_t roIdx,
                       const std::string& entrySymbol, const std::vector<uint32_t>& methodIcSites) {
    if (methodIcSites.empty()) return;

    roSec.align_to(8);
    const size_t sitesOffset = roSec.data.size();
    for (uint32_t site : methodIcSites) {
        roSec.emit64(static_cast<uint64_t>(site));
    }
    defineSymbol(obj, moduleSymbolName(entrySymbol, "__bronze_method_ic_sites"), roIdx, sitesOffset,
                 roSec.data.size() - sitesOffset, SymbolBinding::Local);
}

void emitSourceTextsAndEntries(ObjectFile& obj, Section& roSec, int32_t roIdx,
                               const std::string& entrySymbol, const il::Module& module,
                               const std::vector<std::string>& uniqueNames) {
    for (uint16_t file = 0; file < module.sourceTexts.size(); ++file) {
        uint32_t entryCount = 0;
        for (size_t i = 0; i < module.functions.size(); ++i) {
            const auto& fn = module.functions[i];
            if (fn.sourceFile == file && fn.sourceEnd > fn.sourceBegin && uniqueNames[i] != "main") {
                entryCount++;
            }
        }
        if (entryCount == 0) continue;

        emitStringSymbol(obj, roSec, roIdx,
                         moduleSymbolName(entrySymbol, "__bronze_source_text_" + std::to_string(file)),
                         module.sourceTexts[file], SymbolBinding::Local);

        roSec.align_to(8);
        const size_t entriesOffset = roSec.data.size();
        for (size_t i = 0; i < module.functions.size(); ++i) {
            const auto& fn = module.functions[i];
            if (fn.sourceFile != file || fn.sourceEnd <= fn.sourceBegin || uniqueNames[i] == "main") continue;
            emitPointer(roSec, "__wrapper_" + uniqueNames[i]);
            const uint64_t span = (static_cast<uint64_t>(fn.sourceBegin) << 32) |
                                  static_cast<uint64_t>(fn.sourceEnd - fn.sourceBegin);
            roSec.emit64(span);
        }
        defineSymbol(obj, moduleSymbolName(entrySymbol, "__bronze_source_entries_" + std::to_string(file)),
                     roIdx, entriesOffset, entryCount * 16, SymbolBinding::Local);
    }
}

void emitFileStrings(ObjectFile& obj, Section& roSec, int32_t roIdx,
                     const std::string& entrySymbol, const std::vector<std::string>& sourceFiles) {
    emitStringSymbol(obj, roSec, roIdx, moduleSymbolName(entrySymbol, "__bronze_file_str_empty"), "");
    for (size_t f = 0; f < sourceFiles.size(); ++f) {
        emitStringSymbol(obj, roSec, roIdx,
                         moduleSymbolName(entrySymbol, "__bronze_file_str_" + std::to_string(f)),
                         sourceFiles[f]);
    }
}

std::unordered_map<std::string, std::string> emitFunctionDescriptors(
    ObjectFile& obj, Section& roSec, int32_t roIdx,
    const std::string& entrySymbol, const il::Module& module,
    const std::vector<std::string>& uniqueNames) {
    const std::vector<LineTable>& lineTables = module.lineTables;
    std::unordered_map<std::string, std::string> fnToDesc;

    for (size_t i = 0; i < module.functions.size(); ++i) {
        const auto& fn = module.functions[i];
        if (fn.blocks.empty()) continue;

        std::string_view nameStr;
        if (!fn.displayName.empty()) {
            nameStr = fn.displayName;
        } else if (uniqueNames[i] == "main" || fn.name == "main") {
            nameStr = "";
        } else if (fn.name.rfind("__anon_fn", 0) == 0) {
            nameStr = "<anonymous>";
        } else {
            nameStr = fn.name;
        }

        const std::string nameSymName = moduleSymbolName(entrySymbol, "__bronze_fn_name_" + uniqueNames[i]);
        emitStringSymbol(obj, roSec, roIdx, nameSymName, nameStr);

        const std::string fileSymName = (fn.sourceFile < module.sourceFiles.size())
                                            ? moduleSymbolName(entrySymbol, "__bronze_file_str_" + std::to_string(fn.sourceFile))
                                            : moduleSymbolName(entrySymbol, "__bronze_file_str_empty");

        uint32_t line = 1;
        uint32_t col = 1;
        if (fn.sourceFile < lineTables.size()) {
            const SourceBuffer::LineCol lc = lineTables[fn.sourceFile].lineCol(fn.sourceBegin);
            line = lc.line;
            col = lc.column;
        }

        roSec.align_to(8);
        const size_t descOffset = roSec.data.size();
        emitPointer(roSec, nameSymName);
        emitPointer(roSec, fileSymName);
        roSec.emit32(line);
        roSec.emit32(col);
        roSec.emit32(fn.descFlags);
        roSec.emit32(0);
        emitPointer(roSec, (uniqueNames[i] == "main")
                               ? (entrySymbol.empty() ? "main" : entrySymbol)
                               : ("__wrapper_" + uniqueNames[i]));

        const std::string descSym = moduleSymbolName(entrySymbol, "__bronze_fn_desc_" + uniqueNames[i]);
        defineSymbol(obj, descSym, roIdx, descOffset, 40, SymbolBinding::Global);

        const std::string fnName = (uniqueNames[i] == "main")
                                       ? (entrySymbol.empty() ? "main" : entrySymbol)
                                       : uniqueNames[i];
        fnToDesc[fnName] = descSym;
    }

    return fnToDesc;
}

void emitPcTablesAndCodeRanges(
    ObjectFile& obj, Section& roSec, int32_t roIdx,
    const std::string& entrySymbol,
    const std::unordered_map<std::string, std::string>& fnToDesc,
    support::PhaseTimer& timer) {
    std::unordered_map<std::string, const brass::FunctionDebugTable*> debugTableMap;
    for (const auto& dt : obj.debug_tables) {
        debugTableMap[dt.function_name()] = &dt;
    }

    std::unordered_map<std::string, std::string> fnToPcTable;
    for (const auto& cfi : obj.functions) {
        auto itDt = debugTableMap.find(cfi.name);
        if (itDt != debugTableMap.end() && !itDt->second->line_entries().empty()) {
            const auto& entries = itDt->second->line_entries();
            roSec.align_to(4);
            const size_t pcOff = roSec.data.size();
            for (const auto& le : entries) {
                roSec.emit32(le.code_offset);
                roSec.emit32(le.loc.line);
                roSec.emit32(le.loc.column);
            }
            std::string pcSym = moduleSymbolName(entrySymbol, "__bronze_pc_table_" + cfi.name);
            fnToPcTable[cfi.name] = pcSym;
            defineSymbol(obj, std::move(pcSym), roIdx, pcOff,
                         entries.size() * sizeof(bronze_pc_entry), SymbolBinding::Local);
        }
    }
    timer.mark("pc tables");

    const std::string codeRangesSymbol = (entrySymbol == "bronze_main")
                                             ? "bronze_object_code_ranges"
                                             : (entrySymbol + "_code_ranges");
    const std::string codeRangeCountSymbol = (entrySymbol == "bronze_main")
                                                 ? "bronze_object_code_range_count"
                                                 : (entrySymbol + "_code_range_count");

    roSec.align_to(4);
    const size_t countOff = roSec.data.size();
    roSec.emit32(static_cast<uint32_t>(obj.functions.size()));
    defineSymbol(obj, codeRangeCountSymbol, roIdx, countOff, sizeof(uint32_t), SymbolBinding::Global);

    roSec.align_to(8);
    const size_t rangesOff = roSec.data.size();
    for (const auto& cfi : obj.functions) {
        emitPointer(roSec, cfi.name);
        roSec.emit32(static_cast<uint32_t>(cfi.text_size));
        auto itDt = debugTableMap.find(cfi.name);
        roSec.emit32(itDt != debugTableMap.end() ? static_cast<uint32_t>(itDt->second->line_entries().size()) : 0);
        auto itDesc = fnToDesc.find(cfi.name);
        emitPointer(roSec, itDesc != fnToDesc.end() ? itDesc->second : "");
        auto itPc = fnToPcTable.find(cfi.name);
        emitPointer(roSec, itPc != fnToPcTable.end() ? itPc->second : "");
    }
    defineSymbol(obj, codeRangesSymbol, roIdx, rangesOff,
                 obj.functions.size() * sizeof(bronze_code_range), SymbolBinding::Global);
    timer.mark("code ranges");
}

void emitDataSection(ObjectFile& obj, const std::string& entrySymbol, const il::Module& module,
                     size_t globalCacheCount) {
    const std::string dataSecName = ".data";
    Section& dataSec = obj.get_or_create_section(
        dataSecName,
        brass::object::SectionKind::Data,
        brass::object::SectionFlags::Read | brass::object::SectionFlags::Write | brass::object::SectionFlags::Alloc,
        8
    );
    const int32_t dataIdx = obj.get_section_index(dataSecName);

    dataSec.align_to(8);
    const size_t envOffset = dataSec.data.size();
    dataSec.emit64(BRONZE_ABI_UNDEFINED_BITS);
    defineSymbol(obj, moduleSymbolName(entrySymbol, "__bronze_module_env"), dataIdx, envOffset, 8,
                 SymbolBinding::Local);

    dataSec.align_to(4);
    const size_t keyMapOffset = dataSec.data.size();
    const size_t keyCount = module.keyConstants.size();
    const size_t keyMapBytes = std::max<size_t>(keyCount * sizeof(uint32_t), sizeof(uint32_t));
    dataSec.data.resize(dataSec.data.size() + keyMapBytes, 0);
    defineSymbol(obj, moduleSymbolName(entrySymbol, "__bronze_key_map"), dataIdx, keyMapOffset,
                 keyMapBytes, SymbolBinding::Local);

    dataSec.align_to(8);
    const size_t tplOffset = dataSec.data.size();
    const size_t tplCount = std::max<size_t>(1024, static_cast<size_t>(module.templateSiteCount) + 128);
    const size_t tplBytes = tplCount * sizeof(uint64_t);
    dataSec.data.resize(tplOffset + tplBytes);
    for (size_t i = 0; i < tplCount; ++i) {
        *reinterpret_cast<uint64_t*>(&dataSec.data[tplOffset + i * sizeof(uint64_t)]) = BRONZE_ABI_UNDEFINED_BITS;
    }
    defineSymbol(obj, moduleSymbolName(entrySymbol, "__bronze_template_cells"), dataIdx, tplOffset,
                 tplBytes, SymbolBinding::Local);

    dataSec.align_to(8);
    const size_t cacheOffset = dataSec.data.size();
    const size_t cacheCells = std::max<size_t>(globalCacheCount, 1);
    const size_t cacheBytes = cacheCells * sizeof(uint64_t);
    dataSec.data.resize(cacheOffset + cacheBytes);
    for (size_t i = 0; i < cacheCells; ++i) {
        *reinterpret_cast<uint64_t*>(&dataSec.data[cacheOffset + i * sizeof(uint64_t)]) = BRONZE_ABI_NO_EXCEPTION_BITS;
    }
    defineSymbol(obj, moduleSymbolName(entrySymbol, "__bronze_global_cache"), dataIdx, cacheOffset,
                 cacheBytes, SymbolBinding::Local);

    dataSec.align_to(8);
    const size_t icTableOffset = dataSec.data.size();
    const size_t icSites = std::max<size_t>(module.icSiteCount, 1);
    const size_t icTableBytes = icSites * BRONZE_ABI_IC_SITE_SIZE;
    dataSec.data.resize(icTableOffset + icTableBytes, 0);
    defineSymbol(obj, moduleSymbolName(entrySymbol, "__bronze_ic_table"), dataIdx, icTableOffset,
                 icTableBytes, SymbolBinding::Local);

    const auto& imports = module.nativeImports;
    const std::string importsSymbol = entrySymbol + "_native_imports";
    dataSec.align_to(8);
    const size_t importsOffset = dataSec.data.size();
    dataSec.emit32(static_cast<uint32_t>(imports.size()));
    dataSec.emit32(static_cast<uint32_t>(8 + imports.size() * sizeof(uint64_t)));
    for (size_t i = 0; i < imports.size(); ++i) {
        const bool isClassSlot = imports[i].name.rfind("class ", 0) == 0;
        emitPointer(dataSec, isClassSlot ? "" : "bronze_native_unbound");
    }
    for (const auto& imp : imports) {
        dataSec.emit_bytes(reinterpret_cast<const uint8_t*>(imp.name.data()), imp.name.size());
        dataSec.emit8(0);
        dataSec.emit_bytes(reinterpret_cast<const uint8_t*>(imp.signature.data()), imp.signature.size());
        dataSec.emit8(0);
    }
    defineSymbol(obj, importsSymbol, dataIdx, importsOffset, dataSec.data.size() - importsOffset,
                 SymbolBinding::Global);
    if (!obj.find_symbol("bronze_native_unbound")) {
        ObjectSymbol unbound;
        unbound.name = "bronze_native_unbound";
        unbound.section_index = brass::object::SECTION_UNDEF;
        unbound.binding = SymbolBinding::Global;
        unbound.type = SymbolType::Function;
        obj.add_symbol(std::move(unbound));
    }
}

void demoteInternalSymbols(ObjectFile& obj, const std::string& entrySymbol) {
    const std::string stampSymbol = (entrySymbol == "bronze_main")
                                        ? "bronze_object_abi_fingerprint"
                                        : (entrySymbol + "_abi_fingerprint");
    const std::string manifestSymbol = entrySymbol + "_host_globals";
    const std::string keySymbol = (entrySymbol == "bronze_main")
                                      ? "bronze_main_key_constants"
                                      : (entrySymbol + "_key_constants");
    const std::string importsSymbol = entrySymbol + "_native_imports";
    const std::string codeRangesSymbol = (entrySymbol == "bronze_main")
                                             ? "bronze_object_code_ranges"
                                             : (entrySymbol + "_code_ranges");
    const std::string codeRangeCountSymbol = (entrySymbol == "bronze_main")
                                                 ? "bronze_object_code_range_count"
                                                 : (entrySymbol + "_code_range_count");

    for (auto& sym : obj.symbols) {
        if (sym.section_index != brass::object::SECTION_UNDEF && sym.section_index >= 0) {
            if (sym.name != entrySymbol &&
                sym.name != stampSymbol &&
                sym.name != manifestSymbol &&
                sym.name != keySymbol &&
                sym.name != importsSymbol &&
                sym.name != codeRangesSymbol &&
                sym.name != codeRangeCountSymbol) {
                sym.binding = SymbolBinding::Local;
            }
        }
    }
}

}  // namespace

void emitBronzeSections(ObjectFile& obj, const brass::Target& target, const SectionInputs& in,
                        support::PhaseTimer& timer) {
    const std::string& entrySymbol = in.entrySymbol;
    const std::string keySymbol = (entrySymbol == "bronze_main")
                                      ? "bronze_main_key_constants"
                                      : (entrySymbol + "_key_constants");
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

    const std::string roSecName =
        target.is_windows() ? ".rdata" : (target.is_macos() ? "__const" : ".rodata");
    Section& roSec = obj.get_or_create_section(
        roSecName,
        brass::object::SectionKind::RoData,
        brass::object::SectionFlags::Read | brass::object::SectionFlags::Alloc,
        16
    );
    const int32_t roIdx = obj.get_section_index(roSecName);

    emitAbiStamp(obj, roSec, roIdx, entrySymbol);
    emitHostGlobalsManifest(obj, roSec, roIdx, entrySymbol, in.hostGlobals);
    emitKeyConstants(obj, roSec, roIdx, keySymbol, in.module.keyConstants);
    emitCensusData(obj, roSec, roIdx, entrySymbol, in.module);
    emitMethodIcSites(obj, roSec, roIdx, entrySymbol, in.methodIcSites);
    emitSourceTextsAndEntries(obj, roSec, roIdx, entrySymbol, in.module, in.uniqueNames);
    emitFileStrings(obj, roSec, roIdx, entrySymbol, in.module.sourceFiles);
    timer.mark("ro tables");

    const auto fnToDesc = emitFunctionDescriptors(obj, roSec, roIdx, entrySymbol, in.module, in.uniqueNames);
    timer.mark("descriptors");

    emitPcTablesAndCodeRanges(obj, roSec, roIdx, entrySymbol, fnToDesc, timer);

    emitDataSection(obj, entrySymbol, in.module, in.globalCacheCount);
    demoteInternalSymbols(obj, entrySymbol);
    timer.mark("data section");

    if (support::timingsEnabled()) {
        size_t lineEntries = 0;
        for (const auto& dt : obj.debug_tables) lineEntries += dt.line_entries().size();
        size_t relocs = 0;
        for (const auto& sec : obj.sections) relocs += sec.relocations.size();
        std::fprintf(stderr, "    functions %zu, symbols %zu, relocations %zu, line entries %zu\n",
                     obj.functions.size(), obj.symbols.size(), relocs, lineEntries);
    }
}

}  // namespace bronze::codegen
