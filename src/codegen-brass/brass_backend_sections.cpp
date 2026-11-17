#include "codegen-brass/brass_backend_sections.h"

#include "abi/bronze_abi.h"
#include "codegen-brass/deopt_sites.h"
#include "codegen-brass/size_report.h"
#include "runtime/pc_table.h"
#include "codegen-brass/elem_cells.h"
#include "codegen-brass/il2mir/il_abi.h"
#include "il/feedback_site.h"
#include "support/source.h"

#include <brass/gc/stack_map.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bronze::codegen {

std::string moduleSymbolName(const std::string& entrySymbol, const std::string& base) {
    if (entrySymbol.empty() || entrySymbol == "main" || entrySymbol == "bronze_main") {
        return base;
    }
    return base + "_" + entrySymbol;
}

bool spentHarvest(const il::Module& module) {
    if (!module.shapeCells.empty()) return true;
    return std::any_of(module.functions.begin(), module.functions.end(),
                       [](const il::Function& fn) { return fn.isSpeculated(); });
}

uint32_t coldEntryCount(const il::Module& module) {
    const bool any = std::any_of(module.functions.begin(), module.functions.end(), [](const il::Function& fn) {
        return fn.heat == il::Function::Heat::Never || fn.heat == il::Function::Heat::Entered;
    });
    return any ? static_cast<uint32_t>(module.functions.size()) : 0u;
}

std::string coldEntriesSymbol(const std::string& entrySymbol) { return entrySymbol + "_cold_entries"; }

std::string icIdentitySymbol(const std::string& entrySymbol) {
    return entrySymbol.empty() || entrySymbol == "main" || entrySymbol == "bronze_main"
               ? "bronze_object_ic_identity"
               : entrySymbol + "_ic_identity";
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

// The shape cells' keys (il2mir::kShapeCellKeysSymbol), in cell order: each
// a u32 byte length and its bytes, which init hands
// bronze_register_shape_cells with the cells.
void emitShapeCellKeys(ObjectFile& obj, Section& roSec, int32_t roIdx, const std::string& entrySymbol,
                       const il::Module& module) {
    if (module.shapeCells.empty()) return;
    roSec.align_to(4);
    const size_t at = roSec.data.size();
    for (const il::Module::ShapeCell& cell : module.shapeCells) {
        roSec.emit32(static_cast<uint32_t>(cell.key.size()));
        roSec.emit_bytes(reinterpret_cast<const uint8_t*>(cell.key.data()), cell.key.size());
    }
    defineSymbol(obj, moduleSymbolName(entrySymbol, std::string(il2mir::kShapeCellKeysSymbol)), roIdx, at,
                 roSec.data.size() - at, SymbolBinding::Local);
}

// The double-slot keys (il2mir::kNumberSlotKeysSymbol): u32 key constant
// indices, which init hands bronze_register_slot_repr.
void emitNumberSlotKeys(ObjectFile& obj, Section& roSec, int32_t roIdx, const std::string& entrySymbol,
                        const il::Module& module) {
    if (module.numberSlotKeys.empty()) return;
    roSec.align_to(4);
    const size_t at = roSec.data.size();
    for (uint32_t k : module.numberSlotKeys) roSec.emit32(k);
    defineSymbol(obj, moduleSymbolName(entrySymbol, std::string(il2mir::kNumberSlotKeysSymbol)), roIdx, at,
                 roSec.data.size() - at, SymbolBinding::Local);
}

// The IC IDENTITY record (bronze_abi_layout.h): what a harvest of the module's
// inline caches is filed under. Local under the module's own name, which init
// hands `bronze_register_ic_hot`, and exported for a host that loaded the
// image (eval::LoadedModule).
void emitIcIdentity(ObjectFile& obj, Section& roSec, int32_t roIdx, const std::string& entrySymbol,
                    const il::Module& module, bool counted) {
    roSec.align_to(8);
    const size_t at = roSec.data.size();
    roSec.emit32(BRONZE_ABI_IC_IDENTITY_VERSION);
    roSec.emit32(spentHarvest(module) ? BRONZE_ABI_IC_IDENTITY_PROFILED : 0u);
    roSec.emit32(module.icSiteCount);
    roSec.emit32(static_cast<uint32_t>(module.harvestOutPath.size()));
    roSec.emit64(module.icLayoutDigest);
    // The slow sites' counters, right before the IC table: none in an image
    // whose code counts nothing (emitDataSection).
    roSec.emit32(counted ? module.slowSiteCount : 0u);
    roSec.emit_bytes(reinterpret_cast<const uint8_t*>(module.harvestOutPath.data()), module.harvestOutPath.size());
    roSec.emit8(0);
    const size_t size = roSec.data.size() - at;
    defineSymbol(obj, moduleSymbolName(entrySymbol, "__bronze_ic_identity"), roIdx, at, size, SymbolBinding::Local);
    defineSymbol(obj, icIdentitySymbol(entrySymbol), roIdx, at, size, SymbolBinding::Global);
}

void emitSourceTextsAndEntries(ObjectFile& obj, Section& roSec, int32_t roIdx,
                               const std::string& entrySymbol, const il::Module& module,
                               const std::vector<std::string>& uniqueNames) {
    for (uint16_t file = 0; file < module.sourceTexts.size(); ++file) {
        // A coroutine body shares its stub's source span, and the stub is the
        // function object `toString` is asked about; the body has no wrapper.
        auto hasEntry = [&](size_t i) {
            const auto& fn = module.functions[i];
            return fn.sourceFile == file && fn.sourceEnd > fn.sourceBegin &&
                   uniqueNames[i] != "main" && !fn.isCoroutineBody();
        };
        uint32_t entryCount = 0;
        for (size_t i = 0; i < module.functions.size(); ++i) {
            if (hasEntry(i)) entryCount++;
        }
        if (entryCount == 0) continue;

        emitStringSymbol(obj, roSec, roIdx,
                         moduleSymbolName(entrySymbol, "__bronze_source_text_" + std::to_string(file)),
                         module.sourceTexts[file], SymbolBinding::Local);

        roSec.align_to(8);
        const size_t entriesOffset = roSec.data.size();
        for (size_t i = 0; i < module.functions.size(); ++i) {
            const auto& fn = module.functions[i];
            if (!hasEntry(i)) continue;
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

        // A coroutine body is named in a stack trace as the function it is
        // the body of.
        std::string_view logicalName = fn.name;
        if (fn.isCoroutineBody() && logicalName.size() > 5 &&
            logicalName.substr(logicalName.size() - 5) == ".body") {
            logicalName.remove_suffix(5);
        }
        std::string_view nameStr;
        if (!fn.displayName.empty()) {
            nameStr = fn.displayName;
        } else if (uniqueNames[i] == "main" || logicalName == "main") {
            nameStr = "";
        } else if (logicalName.rfind("__anon_fn", 0) == 0) {
            nameStr = "<anonymous>";
        } else {
            nameStr = logicalName;
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
        roSec.emit32(0);  // the padding before `code`
        // A coroutine body is entered only by brass's resume, never through a
        // call wrapper, so its descriptor names the body itself.
        emitPointer(roSec, (uniqueNames[i] == "main")
                               ? (entrySymbol.empty() ? "main" : entrySymbol)
                               : fn.isCoroutineBody() ? uniqueNames[i]
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

namespace {

// One code range's inline frames (bronze_inline_frame), built from the brass
// inlined scopes its line entries name. A scope whose callee has no function
// descriptor (a runtime-internal function brass's own inliner moved) names no
// frame: its positions are reported as its call's, in the frame around it,
// which is what a trace of the uninlined code would show for a function that
// never appears in one.
class InlineFrameTable {
public:
    struct Frame {
        std::string desc;
        brass::DebugLoc call;
        uint32_t parent = 0;
    };

    InlineFrameTable(const brass::DebugContext& ctx, const std::unordered_map<std::string, std::string>& fnToDesc)
        : ctx_(ctx), fnToDesc_(fnToDesc) {}

    // Rewrites `loc` past any scope with no descriptor and answers the frame
    // (1-based into frames(), 0 for the range's own code) it is a position of.
    uint32_t frameOf(brass::DebugLoc& loc) {
        uint32_t sid = loc.inlined_at_id;
        // Bounded by the scope count: a parent id is always an earlier scope.
        for (size_t guard = 0; sid != 0 && guard <= ctx_.inlined_scopes().size(); ++guard) {
            const brass::InlinedScope* scope = ctx_.get_inlined_scope(sid);
            if (!scope) return 0;
            if (fnToDesc_.count(scope->callee_name)) return frameFor(sid);
            loc = scope->callsite_loc;
            sid = scope->parent_inlined_at_id;
        }
        return 0;
    }

    const std::vector<Frame>& frames() const { return frames_; }

private:
    uint32_t frameFor(uint32_t sid) {
        if (auto it = local_.find(sid); it != local_.end()) return it->second;
        const brass::InlinedScope* scope = ctx_.get_inlined_scope(sid);
        brass::DebugLoc call = scope->callsite_loc;
        call.inlined_at_id = scope->parent_inlined_at_id;
        const uint32_t parent = frameOf(call);
        frames_.push_back(Frame{fnToDesc_.at(scope->callee_name), call, parent});
        const uint32_t index = static_cast<uint32_t>(frames_.size());
        local_.emplace(sid, index);
        return index;
    }

    const brass::DebugContext& ctx_;
    const std::unordered_map<std::string, std::string>& fnToDesc_;
    std::unordered_map<uint32_t, uint32_t> local_;
    std::vector<Frame> frames_;
};

}  // namespace

namespace {
// brass::resume_twin_name's names (il2mir/il_lowering_resume.cpp).
constexpr std::string_view kTwinSuffix = "__twin";
bool isResumeTwinName(std::string_view name) {
    return name.size() > kTwinSuffix.size() && name.substr(name.size() - kTwinSuffix.size()) == kTwinSuffix;
}
std::string twinFunctionName(std::string_view name) {
    return std::string(name.substr(0, name.size() - kTwinSuffix.size()));
}
}  // namespace

void emitPcTablesAndCodeRanges(
    ObjectFile& obj, Section& roSec, int32_t roIdx,
    const std::string& entrySymbol, const std::vector<std::string>& sourceFiles,
    const std::unordered_map<std::string, std::string>& fnToDesc,
    const std::unordered_set<std::string>& stubs, support::PhaseTimer& timer) {
    std::unordered_map<std::string, const brass::FunctionDebugTable*> debugTableMap;
    for (const auto& dt : obj.debug_tables) {
        debugTableMap[dt.function_name()] = &dt;
    }

    // The module's file table, which every code range points at and a pc
    // entry's `file` indexes (bronze_pc_entry): the file strings of
    // emitFileStrings, in il::Module::sourceFiles order. A line entry's
    // debug-context file id maps to its index here; one that names no source
    // file is BRONZE_PC_FILE_DESC, the descriptor's file.
    std::unordered_map<uint32_t, uint32_t> fileIdToIndex;
    for (size_t f = 0; f < sourceFiles.size(); ++f) {
        const uint32_t id = obj.debug_context.get_file_id(sourceFiles[f]);
        if (id != 0) fileIdToIndex.emplace(id, static_cast<uint32_t>(f));
    }
    const std::string filesSym = moduleSymbolName(entrySymbol, "__bronze_file_table");
    if (!sourceFiles.empty()) {
        roSec.align_to(8);
        const size_t filesOff = roSec.data.size();
        for (size_t f = 0; f < sourceFiles.size(); ++f) {
            emitPointer(roSec, moduleSymbolName(entrySymbol, "__bronze_file_str_" + std::to_string(f)));
        }
        defineSymbol(obj, filesSym, roIdx, filesOff, sourceFiles.size() * sizeof(const char*),
                     SymbolBinding::Local);
    }

    const auto fileIndex = [&](uint32_t fileId) {
        auto itFile = fileIdToIndex.find(fileId);
        return itFile != fileIdToIndex.end() ? itFile->second : BRONZE_PC_FILE_DESC;
    };
    std::unordered_map<std::string, std::string> fnToPcTable;
    std::unordered_map<std::string, std::pair<std::string, uint32_t>> fnToInlineFrames;
    for (const auto& cfi : obj.functions) {
        auto itDt = debugTableMap.find(cfi.name);
        if (itDt != debugTableMap.end() && !itDt->second->line_entries().empty()) {
            const auto& entries = itDt->second->line_entries();
            InlineFrameTable frames(obj.debug_context, fnToDesc);
            std::vector<bronze_pc_entry> pcs;
            pcs.reserve(entries.size());
            for (const auto& le : entries) {
                brass::DebugLoc loc = le.loc;
                const uint32_t frame = frames.frameOf(loc);
                pcs.push_back({static_cast<uint32_t>(le.code_offset), loc.line, loc.column, fileIndex(loc.file_id),
                               frame});
            }
            const std::vector<uint8_t> encoded = pctable::encode(std::move(pcs));
            const size_t pcOff = roSec.data.size();
            roSec.emit_bytes(encoded);
            std::string pcSym = moduleSymbolName(entrySymbol, "__bronze_pc_table_" + cfi.name);
            fnToPcTable[cfi.name] = pcSym;
            defineSymbol(obj, std::move(pcSym), roIdx, pcOff, encoded.size(), SymbolBinding::Local);
            if (!frames.frames().empty()) {
                roSec.align_to(8);
                const size_t inlOff = roSec.data.size();
                for (const auto& f : frames.frames()) {
                    emitPointer(roSec, f.desc);
                    roSec.emit32(f.call.line);
                    roSec.emit32(f.call.column);
                    roSec.emit32(fileIndex(f.call.file_id));
                    roSec.emit32(f.parent);
                }
                std::string inlSym = moduleSymbolName(entrySymbol, "__bronze_inline_frames_" + cfi.name);
                defineSymbol(obj, inlSym, roIdx, inlOff, frames.frames().size() * sizeof(bronze_inline_frame),
                             SymbolBinding::Local);
                fnToInlineFrames[cfi.name] = {std::move(inlSym), static_cast<uint32_t>(frames.frames().size())};
            }
        }
    }
    timer.mark("pc tables");

    // Each function's stack map, encoded on its own (bronze_code_range::
    // stack_map), so a loaded image registers exactly the functions it holds.
    std::unordered_map<std::string, std::pair<std::string, size_t>> fnToStackMap;
    for (const auto& fnMap : obj.stack_maps.functions()) {
        if (fnMap.records.empty()) continue;
        brass::ModuleStackMap one;
        one.add_function(fnMap);
        const std::vector<uint8_t> bytes = brass::encode_stack_maps(one);
        roSec.align_to(8);
        const size_t mapOff = roSec.data.size();
        roSec.emit_bytes(bytes);
        std::string mapSym = moduleSymbolName(entrySymbol, "__bronze_stack_map_" + fnMap.function_name);
        defineSymbol(obj, mapSym, roIdx, mapOff, bytes.size(), SymbolBinding::Local);
        fnToStackMap[fnMap.function_name] = {std::move(mapSym), bytes.size()};
    }
    timer.mark("stack maps");

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
        // A resume twin is the function it finishes: the frame a stack walk
        // passes over at the twin's caller, as it does the speculated
        // function's.
        const bool twin = isResumeTwinName(cfi.name);
        // An interpreted function's stub is no frame of its own: the frame
        // is the companion's code it calls (cold_companion.h).
        auto itDesc = stubs.count(cfi.name) ? fnToDesc.end()
                                            : fnToDesc.find(twin ? twinFunctionName(cfi.name) : cfi.name);
        emitPointer(roSec, itDesc != fnToDesc.end() ? itDesc->second : "");
        auto itPc = fnToPcTable.find(cfi.name);
        emitPointer(roSec, itPc != fnToPcTable.end() ? itPc->second : "");
        emitPointer(roSec, sourceFiles.empty() ? "" : filesSym);
        roSec.emit32(static_cast<uint32_t>(sourceFiles.size()));
        auto itMap = fnToStackMap.find(cfi.name);
        roSec.emit32(itMap != fnToStackMap.end() ? static_cast<uint32_t>(itMap->second.second) : 0);
        emitPointer(roSec, itMap != fnToStackMap.end() ? itMap->second.first : "");
        auto itInl = fnToInlineFrames.find(cfi.name);
        emitPointer(roSec, itInl != fnToInlineFrames.end() ? itInl->second.first : "");
        roSec.emit32(itInl != fnToInlineFrames.end() ? itInl->second.second : 0);
        roSec.emit32(twin ? BRONZE_CODE_RANGE_TWIN : 0);
    }
    defineSymbol(obj, codeRangesSymbol, roIdx, rangesOff,
                 obj.functions.size() * sizeof(bronze_code_range), SymbolBinding::Global);
    timer.mark("code ranges");
}

void emitDataSection(ObjectFile& obj, const std::string& entrySymbol, const il::Module& module,
                     size_t globalCacheCount, const std::vector<uint64_t>* slowArmed, size_t deoptWords,
                     const std::vector<std::string>& entryCells) {
    const std::string dataSecName = ".data";
    Section& dataSec = obj.get_or_create_section(
        dataSecName,
        brass::object::SectionKind::Data,
        brass::object::SectionFlags::Read | brass::object::SectionFlags::Write | brass::object::SectionFlags::Alloc,
        8
    );
    const int32_t dataIdx = obj.get_section_index(dataSecName);

    // SHARED by every thread that runs the image: the slot cell
    // bronze_module_instance fills once, and the key remap, whose entries are
    // process-wide interned ids and so the same on every thread.
    dataSec.align_to(8);
    const size_t slotOffset = dataSec.data.size();
    dataSec.emit64(0);
    defineSymbol(obj, moduleSymbolName(entrySymbol, "__bronze_module_slot"), dataIdx, slotOffset, 8,
                 SymbolBinding::Local);

    // The entry cells (deopt_sites.h DeoptSites::retirable): S's address
    // until its G's twin retires it, for every thread at once, as G is what
    // S computes. A function the image does not hold has no calls left.
    std::unordered_set<std::string> held;
    for (const auto& cfi : obj.functions) held.insert(cfi.name);
    for (const std::string& s : entryCells) {
        const size_t at = dataSec.data.size();
        emitPointer(dataSec, held.count(s) ? s : std::string());
        defineSymbol(obj, moduleSymbolName(entrySymbol, il2mir::entry_cell_name(s)), dataIdx, at, 8, SymbolBinding::Local);
    }

    dataSec.align_to(4);
    const size_t keyMapOffset = dataSec.data.size();
    const size_t keyCount = module.keyConstants.size();
    const size_t keyMapBytes = std::max<size_t>(keyCount * sizeof(uint32_t), sizeof(uint32_t));
    dataSec.data.resize(dataSec.data.size() + keyMapBytes, 0);
    defineSymbol(obj, moduleSymbolName(entrySymbol, "__bronze_key_map"), dataIdx, keyMapOffset,
                 keyMapBytes, SymbolBinding::Local);

    // PER THREAD from here to `__bronze_instance_end`: every table that holds
    // a Value or a thread's registration. The home thread uses these bytes;
    // every other thread a copy of them taken before the home thread wrote
    // any (bronze_abi.h, bronze_module_instance). Every table's offset from
    // the run's start is a multiple of its alignment, and the copies are
    // allocated at least that aligned, so a table is aligned in every copy.
    dataSec.align_to(64);
    const size_t instanceOffset = dataSec.data.size();

    dataSec.align_to(8);
    const size_t envOffset = dataSec.data.size();
    dataSec.emit64(BRONZE_ABI_UNDEFINED_BITS);
    defineSymbol(obj, moduleSymbolName(entrySymbol, "__bronze_module_env"), dataIdx, envOffset, 8,
                 SymbolBinding::Local);

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
        *reinterpret_cast<uint64_t*>(&dataSec.data[cacheOffset + i * sizeof(uint64_t)]) = BRONZE_ABI_HOLE_BITS;
    }
    defineSymbol(obj, moduleSymbolName(entrySymbol, "__bronze_global_cache"), dataIdx, cacheOffset,
                 cacheBytes, SymbolBinding::Local);

    // The shape cells (il.h Module::shapeCells; il2mir::kShapeCellsSymbol):
    // zero, a word no object carries, until the runtime fills one with the
    // shape its key names. Per thread, as shapes are.
    if (!module.shapeCells.empty()) {
        dataSec.align_to(8);
        const size_t cellsOffset = dataSec.data.size();
        const size_t cellsBytes = module.shapeCells.size() * sizeof(uint64_t);
        dataSec.data.resize(cellsOffset + cellsBytes, 0);
        defineSymbol(obj, moduleSymbolName(entrySymbol, std::string(il2mir::kShapeCellsSymbol)), dataIdx,
                     cellsOffset, cellsBytes, SymbolBinding::Local);
    }

    // The cold entries (il2mir::kColdEntriesSymbol): the word count, then a
    // zero word per function, which a cold one bumps on entry. Exported at
    // the count for the host that runs the module.
    if (const uint32_t cold = coldEntryCount(module); cold != 0) {
        dataSec.align_to(8);
        const size_t countOffset = dataSec.data.size();
        dataSec.data.resize(countOffset + (size_t{1} + cold) * sizeof(uint64_t), 0);
        const uint64_t words = cold;
        std::memcpy(dataSec.data.data() + countOffset, &words, sizeof(words));
        defineSymbol(obj, moduleSymbolName(entrySymbol, std::string(il2mir::kColdEntriesSymbol)), dataIdx,
                     countOffset + sizeof(uint64_t), cold * sizeof(uint64_t), SymbolBinding::Local);
        defineSymbol(obj, coldEntriesSymbol(entrySymbol), dataIdx, countOffset,
                     (size_t{1} + cold) * sizeof(uint64_t), SymbolBinding::Global);
    }

    // The literal shapes (il2mir::kLiteralShapesSymbol): zero, per thread.
    if (!module.objectLiterals.empty()) {
        dataSec.align_to(8);
        const size_t litOffset = dataSec.data.size();
        const size_t litBytes = module.objectLiterals.size() * 2 * sizeof(uint64_t);
        dataSec.data.resize(litOffset + litBytes, 0);
        defineSymbol(obj, moduleSymbolName(entrySymbol, std::string(il2mir::kLiteralShapesSymbol)), dataIdx,
                     litOffset, litBytes, SymbolBinding::Local);
    }

    // The pretenured sites' records (il2mir::kPretenureSitesSymbol): zero.
    // Code reads the home instance's, which every thread shares.
    if (!module.pretenureSites.empty()) {
        dataSec.align_to(8);
        const size_t sitesOffset = dataSec.data.size();
        const size_t sitesBytes = module.pretenureSites.size() * il2mir::kPretenureSiteBytes;
        dataSec.data.resize(sitesOffset + sitesBytes, 0);
        defineSymbol(obj, moduleSymbolName(entrySymbol, std::string(il2mir::kPretenureSitesSymbol)), dataIdx,
                     sitesOffset, sitesBytes, SymbolBinding::Local);
    }

    // The transition cells (il2mir::kProtoCellsSymbol): zero, per thread.
    if (!module.protoCells.empty()) {
        dataSec.align_to(8);
        const size_t cellOffset = dataSec.data.size();
        const size_t cellBytes = module.protoCells.size() * sizeof(uint64_t);
        dataSec.data.resize(cellOffset + cellBytes, 0);
        defineSymbol(obj, moduleSymbolName(entrySymbol, std::string(il2mir::kProtoCellsSymbol)), dataIdx,
                     cellOffset, cellBytes, SymbolBinding::Local);
    }

    // The member cells (il2mir::kMemberCellsSymbol): zero, per thread.
    if (!module.memberCells.empty()) {
        dataSec.align_to(8);
        const size_t cellOffset = dataSec.data.size();
        const size_t cellBytes = module.memberCells.size() * 2 * sizeof(uint64_t);
        dataSec.data.resize(cellOffset + cellBytes, 0);
        defineSymbol(obj, moduleSymbolName(entrySymbol, std::string(il2mir::kMemberCellsSymbol)), dataIdx,
                     cellOffset, cellBytes, SymbolBinding::Local);
    }

    // The element cells (il2mir::kElemCellsSymbol): zero, per thread.
    if (const size_t cells = elemCellSites(module).size(); cells != 0) {
        dataSec.align_to(8);
        const size_t cellOffset = dataSec.data.size();
        const size_t cellBytes = cells * il2mir::kElemCellBytes;
        dataSec.data.resize(cellOffset + cellBytes, 0);
        defineSymbol(obj, moduleSymbolName(entrySymbol, std::string(il2mir::kElemCellsSymbol)), dataIdx,
                     cellOffset, cellBytes, SymbolBinding::Local);
    }

    // The deopt counters (codegen-brass/deopt_sites.h): zero, per thread.
    if (deoptWords != 0) {
        dataSec.align_to(8);
        const size_t at = dataSec.data.size();
        dataSec.data.resize(at + deoptWords * sizeof(uint64_t), 0);
        defineSymbol(obj, moduleSymbolName(entrySymbol, "__bronze_deopt_counts"), dataIdx, at,
                     deoptWords * sizeof(uint64_t), SymbolBinding::Local);
    }

    // The slow sites' counters (il.h `slowSiteCount`), zero, ending where the
    // table starts (il2mir::ic_slow_count_offset): generated code bumps one
    // off the table's base, and the harvest finds them by their count, which
    // the identity record carries. Right before them the ARMED bitmap: bit i
    // of word i / 64 set when the code counts site i (`slowArmed`, from
    // lowering; all clear in a build that counts nothing), which the harvest
    // writes beside the counts (an in-process image's lazy bodies arm theirs
    // into the loaded words: il2mir::SlowArmedBits). An image whose code
    // counts nothing (built on a harvest, which is never harvested again, or
    // in process and not to be harvested: `slowArmed` null) has neither, and
    // its identity record says so.
    if (module.slowSiteCount != 0 && slowArmed != nullptr) {
        const size_t armedWords = (static_cast<size_t>(module.slowSiteCount) + 63) / 64;
        const size_t bytes = (armedWords + module.slowSiteCount) * sizeof(uint64_t);
        const size_t end = (dataSec.data.size() + bytes + 63) & ~size_t{63};
        dataSec.data.resize(end, 0);
        const size_t armedAt = end - bytes;
        for (size_t w = 0; w < armedWords && w < slowArmed->size(); ++w) {
            std::memcpy(dataSec.data.data() + armedAt + w * sizeof(uint64_t), &(*slowArmed)[w], sizeof(uint64_t));
        }
        defineSymbol(obj, moduleSymbolName(entrySymbol, "__bronze_slow_counts"), dataIdx,
                     armedAt + armedWords * sizeof(uint64_t), module.slowSiteCount * sizeof(uint64_t),
                     SymbolBinding::Local);
    }
    // Line-aligned, and the packed rows a fixed distance after it
    // (il2mir::ic_hot_table_offset): generated code reads a row off the
    // table's base.
    dataSec.align_to(64);
    const size_t icTableOffset = dataSec.data.size();
    const size_t icSites = std::max<size_t>(module.icSiteCount, 1);
    const size_t icTableBytes = icSites * BRONZE_ABI_IC_SITE_SIZE;
    dataSec.data.resize(icTableOffset + icTableBytes, 0);
    defineSymbol(obj, moduleSymbolName(entrySymbol, "__bronze_ic_table"), dataIdx, icTableOffset,
                 icTableBytes, SymbolBinding::Local);

    // Every site's way-0 shape and slot words, packed in site order
    // (bronze_abi_layout.h, "the HOT array"): what the way-0 guards read.
    // Zero, like the table: an empty row matches no receiver.
    const size_t icHotOffset = icTableOffset + static_cast<size_t>(il2mir::ic_hot_table_offset(icSites));
    const size_t icHotBytes = icSites * BRONZE_ABI_IC_HOT_ENTRY_SIZE;
    dataSec.data.resize(icHotOffset + icHotBytes, 0);
    defineSymbol(obj, moduleSymbolName(entrySymbol, "__bronze_ic_hot"), dataIdx, icHotOffset, icHotBytes,
                 SymbolBinding::Local);

    // The method-call sites' way-0 guard words, one row per site of the
    // sorted method-site list (bronze_abi_layout.h, "the METHOD ROWS"), a
    // fixed distance after the table: zero, an unlatched way 0.
    const size_t methodRowsOffset = icTableOffset + static_cast<size_t>(il2mir::ic_method_rows_offset(icSites));
    const size_t methodRowsBytes = std::max<size_t>(module.methodIcSites().size(), 1) * BRONZE_ABI_METHOD_ROW_SIZE;
    dataSec.data.resize(methodRowsOffset + methodRowsBytes, 0);
    defineSymbol(obj, moduleSymbolName(entrySymbol, "__bronze_ic_method_rows"), dataIdx, methodRowsOffset,
                 methodRowsBytes, SymbolBinding::Local);

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

    // The run's bounds. The end is a real word rather than a zero-size
    // symbol at the section's edge, which not every image writer places.
    dataSec.align_to(8);
    const size_t instanceEnd = dataSec.data.size();
    dataSec.emit64(0);
    defineSymbol(obj, moduleSymbolName(entrySymbol, "__bronze_instance"), dataIdx, instanceOffset,
                 instanceEnd - instanceOffset, SymbolBinding::Local);
    defineSymbol(obj, moduleSymbolName(entrySymbol, "__bronze_instance_end"), dataIdx, instanceEnd, 8,
                 SymbolBinding::Local);
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
                sym.name != codeRangeCountSymbol &&
                sym.name != icIdentitySymbol(entrySymbol) &&
                sym.name != coldEntriesSymbol(entrySymbol)) {
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
    emitIcIdentity(obj, roSec, roIdx, entrySymbol, in.module, in.slowArmed != nullptr);
    emitShapeCellKeys(obj, roSec, roIdx, entrySymbol, in.module);
    emitNumberSlotKeys(obj, roSec, roIdx, entrySymbol, in.module);
    emitSourceTextsAndEntries(obj, roSec, roIdx, entrySymbol, in.module, in.uniqueNames);
    emitFileStrings(obj, roSec, roIdx, entrySymbol, in.module.sourceFiles);
    const DeoptSites deopt = collectDeoptSites(in.module, in.uniqueNames);
    if (!deopt.sites.empty()) {
        emitStringSymbol(obj, roSec, roIdx, moduleSymbolName(entrySymbol, "__bronze_deopt_sites"), deopt.text);
    }
    timer.mark("ro tables");

    const auto fnToDesc = emitFunctionDescriptors(obj, roSec, roIdx, entrySymbol, in.module, in.uniqueNames);
    timer.mark("descriptors");

    std::unordered_set<std::string> stubs;
    if (in.interpreted) {
        for (size_t i = 0; i < in.interpreted->size(); ++i) {
            if ((*in.interpreted)[i] != UINT32_MAX) stubs.insert(in.uniqueNames[i]);
        }
    }
    emitPcTablesAndCodeRanges(obj, roSec, roIdx, entrySymbol, in.module.sourceFiles, fnToDesc, stubs, timer);

    std::vector<std::string> entryCells;
    for (const uint32_t s : deopt.retirable) entryCells.push_back(in.uniqueNames[s]);
    emitDataSection(obj, entrySymbol, in.module, in.globalCacheCount, in.slowArmed,
                    deopt.sites.empty() ? 0 : 2 * deopt.generics.size() + deopt.sites.size(), entryCells);
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
    if (sizeReportEnabled()) printSizeReport(obj, in.module, in.uniqueNames, entrySymbol);
}

}  // namespace bronze::codegen
