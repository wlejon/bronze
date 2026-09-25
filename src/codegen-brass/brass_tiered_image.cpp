#include "codegen-brass/brass_tiered_image.h"

#include "codegen-brass/brass_backend_sections.h"
#include "codegen-brass/brass_symbol_registration.h"
#include "embed/embed.h"
#include "support/diagnostics.h"

#include <brass/mir/function.hpp>
#include <brass/vm/fast_interpreter.hpp>

#include <algorithm>

namespace bronze {

namespace {

// Every loaded image by its program's MIR module: what an interpreted
// frame's Function is attributed through.
std::mutex& registryMutex() {
    static auto* mu = new std::mutex();
    return *mu;
}
std::unordered_map<const brass::Module*, const TieredProgramImage*>& registry() {
    static auto* map = new std::unordered_map<const brass::Module*, const TieredProgramImage*>();
    return *map;
}

// runtime/interpreted_frames.h's walker: the Tier-0 frames on this thread
// whose function belongs to a loaded program, innermost first.
size_t walkInterpretedFrames(runtime::InterpretedFrame* out, size_t capacity) {
    size_t n = 0;
    std::lock_guard<std::mutex> lock(registryMutex());
    brass::FastInterpreter::for_each_frame_on_thread([&](const brass::InterpretedFrameInfo& f) {
        if (!f.function) return true;
        auto it = registry().find(f.function->parent());
        if (it == registry().end()) return true;
        const TieredProgramImage& image = *it->second;
        const bronze_fn_desc* desc = image.descriptorOf(f.function->name());
        if (!desc) return true;
        if (n < capacity) {
            const bronze_pc_entry pos = image.translate(f.loc, *desc);
            runtime::InterpretedFrame& frame = out[n];
            frame.stackAddress = reinterpret_cast<uintptr_t>(f.frame_address);
            frame.desc = desc;
            frame.line = pos.line;
            frame.col = pos.col;
            frame.file = image.fileName(pos.file);
        }
        ++n;
        return true;
    });
    return n;
}

}  // namespace

std::unique_ptr<TieredProgramImage> TieredProgramImage::load(const brass::object::ObjectFile& image,
                                                             const brass::Module& module, const il::Module& il,
                                                             const std::string& entrySymbol,
                                                             brass::runtime::MultiTierPipeline& pipeline,
                                                             DiagnosticSink& diags) {
    std::unique_ptr<TieredProgramImage> self(new TieredProgramImage());
    self->module_ = &module;
    self->data_ = std::make_unique<brass::codegen::JitExecutionEngine>(brass::Target::host());
    brass::codegen::JitExecutionEngine& data = *self->data_;
    registerBronzeJitSymbols(data);
    // The image's pointers to the program's functions (descriptors, source
    // entries) are the pipeline's function pointers: what a closure's code
    // pointer is in every tier.
    for (const brass::Function* fn : module.functions()) {
        if (!fn || fn->block_count() == 0) continue;
        data.register_external_symbol(fn->name(), pipeline.function_address(fn->name(), fn));
    }
    if (!data.load_object(image)) {
        diags.error(Span{}, "Failed to load the program's data image");
        return nullptr;
    }

    // Every tier resolves the program's data symbols here.
    for (const auto& sym : image.symbols) {
        if (sym.section_index < 0) continue;
        if (void* addr = data.get_symbol_address(sym.name)) pipeline.register_external_symbol(sym.name, addr);
    }
    // The entry's key table under the name il2mir gives it for a default
    // entry, which an object file's emitter renames.
    const std::string keySymbol =
        (entrySymbol == "bronze_main") ? "bronze_main_key_constants" : (entrySymbol + "_key_constants");
    if (void* keys = data.get_symbol_address(keySymbol)) {
        pipeline.register_external_symbol("bronze_main_key_constants", keys);
    }

    for (const brass::Function* fn : module.functions()) {
        if (!fn || fn->block_count() == 0) continue;
        const std::string mirName(fn->name());
        const std::string unique = (mirName == entrySymbol) ? "main" : mirName;
        const void* desc =
            data.get_symbol_address(codegen::moduleSymbolName(entrySymbol, "__bronze_fn_desc_" + unique));
        if (desc) self->descs_.emplace(mirName, static_cast<const bronze_fn_desc*>(desc));
    }

    // The file table and the MIR's file ids of its files, as
    // translateDebugLocations reads an object's (brass_backend_debug.cpp).
    self->files_ = static_cast<const char* const*>(
        data.get_symbol_address(codegen::moduleSymbolName(entrySymbol, "__bronze_file_table")));
    self->fileCount_ = self->files_ ? static_cast<uint32_t>(il.sourceFiles.size()) : 0;
    const brass::DebugContext& dc = module.debug_context();
    for (size_t f = 0; f < il.sourceFiles.size(); ++f) {
        const uint32_t id = dc.get_file_id(il.sourceFiles[f]);
        if (id != 0) self->fileIndexById_.emplace(id, static_cast<uint32_t>(f));
    }
    const std::string placeholder = !il.name.empty()              ? il.name
                                    : !il.sourceFiles.empty()     ? il.sourceFiles[0]
                                                                  : std::string("<anonymous>");
    if (std::find(il.sourceFiles.begin(), il.sourceFiles.end(), placeholder) == il.sourceFiles.end()) {
        self->placeholderFileId_ = dc.get_file_id(placeholder);
    }

    TieredProgramImage* raw = self.get();
    pipeline.set_code_install_observer(
        [raw](const brass::runtime::InstalledCode& code) { raw->onCodeInstalled(code); });
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        registry()[&module] = raw;
    }
    embed::setInterpretedFrameWalker(&walkInterpretedFrames);
    return self;
}

TieredProgramImage::~TieredProgramImage() {
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        auto it = registry().find(module_);
        if (it != registry().end() && it->second == this) registry().erase(it);
    }
    std::lock_guard<std::mutex> lock(rangesMutex_);
    for (RangeRecord& r : ranges_) bronze_unregister_code_ranges(&r.range, 1);
}

void* TieredProgramImage::symbolAddress(std::string_view name) const {
    return data_ ? data_->get_symbol_address(name) : nullptr;
}

const bronze_fn_desc* TieredProgramImage::descriptorOf(std::string_view mirName) const {
    auto it = descs_.find(std::string(mirName));
    return it != descs_.end() ? it->second : nullptr;
}

bronze_pc_entry TieredProgramImage::translate(const brass::DebugLoc& loc, const bronze_fn_desc& desc) const {
    bronze_pc_entry e{};
    e.file = BRONZE_PC_FILE_DESC;
    if (loc.file_id != 0 && loc.file_id != placeholderFileId_) {
        auto it = fileIndexById_.find(loc.file_id);
        if (it != fileIndexById_.end()) e.file = it->second;
    }
    if (loc.line == 0) {
        e.line = desc.def_line;
        e.col = desc.def_col;
    } else {
        e.line = loc.line;
        e.col = loc.column;
    }
    return e;
}

const char* TieredProgramImage::fileName(uint32_t fileIndex) const {
    return (files_ && fileIndex < fileCount_) ? files_[fileIndex] : nullptr;
}

void TieredProgramImage::onCodeInstalled(const brass::runtime::InstalledCode& code) {
    const bronze_fn_desc* desc = descriptorOf(code.name);
    if (!desc || !code.code || code.size == 0) return;
    std::lock_guard<std::mutex> lock(rangesMutex_);
    RangeRecord& r = ranges_.emplace_back();
    if (code.lines) {
        r.pcs.reserve(code.lines->size());
        for (const brass::DebugLineEntry& le : *code.lines) {
            bronze_pc_entry e = translate(le.loc, *desc);
            e.pc_offset = le.code_offset;
            r.pcs.push_back(e);
        }
    }
    r.range.code_start = code.code;
    r.range.code_size = static_cast<uint32_t>(code.size);
    r.range.pc_count = static_cast<uint32_t>(r.pcs.size());
    r.range.desc = desc;
    r.range.pc_table = r.pcs.empty() ? nullptr : r.pcs.data();
    r.range.files = files_;
    r.range.file_count = fileCount_;
    bronze_register_code_ranges(&r.range, 1);
}

}  // namespace bronze
