#include "codegen-brass/brass_backend_debug.h"

#include <brass/debug/codeview_emitter.hpp>
#include <brass/debug/debug_section.hpp>
#include <brass/debug/dwarf_emitter.hpp>
#include <brass/debug/source_loc.hpp>
#include <brass/object/object_writer.hpp>

#include <filesystem>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace bronze::codegen {

namespace {

void removeSection(brass::object::ObjectFile& obj, std::string_view name) {
    int32_t idx = obj.get_section_index(name);
    if (idx < 0) return;
    obj.sections.erase(obj.sections.begin() + idx);
    for (auto& sym : obj.symbols) {
        if (sym.section_index == idx) {
            sym.section_index = brass::object::SECTION_UNDEF;
        } else if (sym.section_index > idx) {
            sym.section_index--;
        }
    }
}

}  // namespace

void translateDebugLocations(
    brass::object::ObjectFile& obj,
    const il::Module& module,
    const std::vector<std::string>& uniqueNames,
    const std::string& entrySymbol
) {
    std::vector<uint32_t> fileIds;
    fileIds.reserve(module.sourceFiles.size());
    for (const auto& file : module.sourceFiles) {
        fileIds.push_back(obj.debug_context.get_or_add_file(file));
    }
    if (fileIds.empty()) {
        std::string defFile = !module.name.empty() ? module.name : "<anonymous>";
        fileIds.push_back(obj.debug_context.get_or_add_file(defFile));
    }

    std::unordered_map<std::string, size_t> fnMap;
    for (size_t i = 0; i < module.functions.size(); ++i) {
        std::string name = (uniqueNames[i] == "main" && !entrySymbol.empty() && entrySymbol != "main")
                               ? entrySymbol
                               : uniqueNames[i];
        fnMap[name] = i;
    }

    std::unordered_map<std::string, const brass::object::CompiledFunctionInfo*> cfiMap;
    for (const auto& cfi : obj.functions) {
        cfiMap[cfi.name] = &cfi;
    }

    for (auto& dt : obj.debug_tables) {
        auto it = fnMap.find(dt.function_name());
        if (it == fnMap.end()) continue;
        size_t fnIdx = it->second;
        const auto& fn = module.functions[fnIdx];

        uint32_t fileId = (fn.sourceFile < fileIds.size()) ? fileIds[fn.sourceFile] : fileIds[0];

        uint32_t declLine = 1;
        uint32_t declCol = 1;
        if (fn.sourceFile < module.lineTables.size() && (fn.sourceBegin != 0 || fn.sourceEnd != 0)) {
            auto lc = module.lineTables[fn.sourceFile].lineCol(fn.sourceBegin);
            declLine = lc.line;
            declCol = lc.column;
        }

        dt.set_decl_file(fileId);
        dt.set_decl_line(declLine);

        auto itCfi = cfiMap.find(dt.function_name());
        if (itCfi != cfiMap.end()) {
            if (dt.code_size() == 0 && itCfi->second->text_size > 0) {
                dt.set_code_size(static_cast<uint32_t>(itCfi->second->text_size));
                dt.set_prologue_size(static_cast<uint32_t>(itCfi->second->prologue_size));
            }
        }

        if (dt.line_entries().empty()) {
            if (dt.code_size() > 0) {
                dt.add_line_entry(0, brass::DebugLoc(fileId, declLine, declCol));
            }
        } else {
            auto entries = dt.line_entries();
            for (auto& le : entries) {
                if (le.loc.file_id == 0 || le.loc.file_id == 1) {
                    le.loc.file_id = fileId;
                }
                if (le.loc.line == 0) {
                    le.loc.line = declLine;
                    le.loc.column = declCol;
                }
            }
            dt.set_line_entries(std::move(entries));
        }

        if (dt.variables().empty()) {
            for (size_t pIdx = 0; pIdx < fn.params.size(); ++pIdx) {
                brass::DebugVariable var;
                var.name = fn.params[pIdx].name;
                var.is_parameter = true;
                var.decl_file = fileId;
                var.decl_line = declLine;
                var.decl_column = declCol;
                var.stack_offset = static_cast<int32_t>(16 + pIdx * 8);
                dt.add_variable(std::move(var));
            }
        }
    }
}

void emitNativeDebugSections(
    brass::object::ObjectFile& obj,
    const il::Module& module,
    const std::vector<std::string>& uniqueNames,
    const std::string& entrySymbol,
    bool emitDebugInfo
) {
    if (emitDebugInfo) {
        translateDebugLocations(obj, module, uniqueNames, entrySymbol);

        if (obj.target.is_windows()) {
            brass::debug::CodeViewOptions cvOpts;
            if (obj.target.is_aarch64()) {
                cvOpts.is_aarch64 = true;
            }
            brass::debug::CodeViewEmitter::emit(obj, cvOpts);
        } else {
            brass::debug::DwarfOptions dwarfOpts;
            dwarfOpts.target = obj.target;
            dwarfOpts.version = 4;
            dwarfOpts.producer = "bronze 0.1.0";
            if (!module.sourceFiles.empty()) {
                std::filesystem::path p(module.sourceFiles[0]);
                if (p.has_parent_path()) {
                    dwarfOpts.comp_dir = p.parent_path().string();
                }
            }
            brass::debug::DwarfEmitter::emit(obj, dwarfOpts);
        }
    } else {
        removeSection(obj, ".brass_dbg");
        removeSection(obj, ".debug_line");
        removeSection(obj, ".debug_info");
        removeSection(obj, ".debug_abbrev");
        removeSection(obj, ".debug_str");
        removeSection(obj, ".debug$S");
        removeSection(obj, ".debug$T");
        obj.debug_tables.clear();
        obj.debug_context.clear();
    }
}

}  // namespace bronze::codegen
