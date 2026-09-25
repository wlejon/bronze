#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <brass/codegen/jit_exec.hpp>
#include <brass/debug/source_loc.hpp>
#include <brass/mir/module.hpp>
#include <brass/object/object_writer.hpp>
#include <brass/runtime/multi_tier_pipeline.hpp>

#include "abi/bronze_abi.h"
#include "il/il.h"

namespace bronze {

class DiagnosticSink;

// What an object file gives a program beside its code, for a program the
// tiered pipeline runs from MIR: the bronze-owned tables and data cells
// (BrassBackend::buildDataImage), loaded into memory with their references
// to the program's functions bound to the pipeline's function pointers, and
// every symbol in it registered with the pipeline so each tier resolves the
// program's data where the image holds it.
//
// It is also how the runtime's stack walker sees the program: code the
// pipeline installs (Tier 1, Tier 2) is registered as code ranges as it is
// installed, with pc tables made from the compiler's line tables, and the
// frames the Tier-0 interpreter runs are reported through the process's
// interpreted-frame walker (runtime/interpreted_frames.h), both attributed
// to the function descriptors the image holds.
class TieredProgramImage {
public:
    // Loads `image` for `module` (the program's MIR, named after `il`),
    // whose entry is `entrySymbol`, into `pipeline`, and starts observing the
    // code it installs. Null (with a diagnostic) when the image does not load.
    static std::unique_ptr<TieredProgramImage> load(const brass::object::ObjectFile& image,
                                                    const brass::Module& module, const il::Module& il,
                                                    const std::string& entrySymbol,
                                                    brass::runtime::MultiTierPipeline& pipeline,
                                                    DiagnosticSink& diags);
    ~TieredProgramImage();

    TieredProgramImage(const TieredProgramImage&) = delete;
    TieredProgramImage& operator=(const TieredProgramImage&) = delete;

    // A symbol the image defines, or null.
    void* symbolAddress(std::string_view name) const;

    // The descriptor of the program function MIR names `mirName`, or null
    // (a wrapper or a thunk, which is not a JS function of its own).
    const bronze_fn_desc* descriptorOf(std::string_view mirName) const;

    // A source position of the program's MIR as a pc-table entry would give
    // it (brass_backend_debug.cpp, translateDebugLocations): its file as an
    // index into the image's file table (BRONZE_PC_FILE_DESC for the
    // descriptor's own), and the descriptor's position where it has none.
    bronze_pc_entry translate(const brass::DebugLoc& loc, const bronze_fn_desc& desc) const;
    const char* fileName(uint32_t fileIndex) const;

private:
    TieredProgramImage() = default;
    void onCodeInstalled(const brass::runtime::InstalledCode& code);

    struct RangeRecord {
        bronze_code_range range{};
        std::vector<bronze_pc_entry> pcs;
    };

    const brass::Module* module_ = nullptr;
    std::unique_ptr<brass::codegen::JitExecutionEngine> data_;
    std::unordered_map<std::string, const bronze_fn_desc*> descs_;
    std::unordered_map<uint32_t, uint32_t> fileIndexById_;
    uint32_t placeholderFileId_ = 0;
    const char* const* files_ = nullptr;
    uint32_t fileCount_ = 0;

    // Installs arrive on the mutator and on background compiler threads.
    std::mutex rangesMutex_;
    std::deque<RangeRecord> ranges_;
};

}  // namespace bronze
