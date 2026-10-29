#include "codegen-brass/brass_tiered_image.h"

#include "codegen-brass/brass_backend_sections.h"
#include "codegen-brass/brass_symbol_registration.h"
#include "embed/embed.h"
#include "embed/embed_profiler.h"
#include "support/diagnostics.h"
#include "support/timings.h"

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
// whose function belongs to a loaded program, innermost first. A frame at a
// position inside a call compiled in place (lower/inline_guarded.cpp) is
// reported as the frames the call would have made, all at its record's
// address: the stack walker keeps equal addresses in the order given.
size_t walkInterpretedFrames(runtime::InterpretedFrame* out, size_t capacity) {
    size_t n = 0;
    std::vector<TieredProgramImage::JsFrame> js;
    std::lock_guard<std::mutex> lock(registryMutex());
    brass::FastInterpreter::for_each_frame_on_thread([&](const brass::InterpretedFrameInfo& f) {
        if (!f.function) return true;
        auto it = registry().find(f.function->parent());
        if (it == registry().end()) return true;
        const TieredProgramImage& image = *it->second;
        const bronze_fn_desc* desc = image.descriptorOf(f.function->name());
        if (!desc) return true;
        js.clear();
        // The scopes are the program module's: Tier 0 runs its Functions.
        image.expandInlined(f.loc, f.function->parent()->debug_context(), *desc, js);
        for (const TieredProgramImage::JsFrame& one : js) {
            if (n < capacity) {
                runtime::InterpretedFrame& frame = out[n];
                frame.stackAddress = reinterpret_cast<uintptr_t>(f.frame_address);
                frame.desc = one.desc;
                frame.line = one.pos.line;
                frame.col = one.pos.col;
                frame.file = image.fileName(one.pos.file);
            }
            ++n;
        }
        return true;
    });
    return n;
}

// One installed code range's inline frames (bronze_inline_frame), built from
// the scopes its line entries name, as the object writer builds an image's
// (brass_backend_sections.cpp, InlineFrameTable) but with the descriptors
// the loaded image holds.
class InstalledInlineFrames {
public:
    InstalledInlineFrames(const TieredProgramImage& image, const brass::DebugContext& debug,
                          const bronze_fn_desc& desc)
        : image_(image), debug_(debug), desc_(desc) {}

    // Rewrites `loc` past any scope with no descriptor and answers the frame
    // (1-based into frames(), 0 for the range's own code) it is a position
    // of.
    uint32_t frameOf(brass::DebugLoc& loc) {
        uint32_t sid = loc.inlined_at_id;
        // Bounded by the scope count: a parent is always an earlier scope.
        for (size_t guard = 0; sid != 0 && guard <= debug_.inlined_scope_count(); ++guard) {
            const brass::InlinedScope* scope = debug_.get_inlined_scope(sid);
            if (!scope) return 0;
            if (const bronze_fn_desc* callee = image_.descriptorOf(scope->callee_name)) return frameFor(sid, *scope, callee);
            loc = scope->callsite_loc;
            sid = scope->parent_inlined_at_id;
        }
        return 0;
    }

    std::vector<bronze_inline_frame>& frames() { return frames_; }

private:
    uint32_t frameFor(uint32_t sid, const brass::InlinedScope& scope, const bronze_fn_desc* callee) {
        if (auto it = local_.find(sid); it != local_.end()) return it->second;
        brass::DebugLoc call = scope.callsite_loc;
        call.inlined_at_id = scope.parent_inlined_at_id;
        const uint32_t parent = frameOf(call);
        // The call's position is in the frame around it, whose descriptor
        // gives the position a line when the call has none.
        const bronze_fn_desc& around = parent ? *frames_[parent - 1].desc : desc_;
        const bronze_pc_entry pos = image_.translate(call, around);
        frames_.push_back(bronze_inline_frame{callee, pos.line, pos.col, pos.file, parent});
        const uint32_t index = static_cast<uint32_t>(frames_.size());
        local_.emplace(sid, index);
        return index;
    }

    const TieredProgramImage& image_;
    const brass::DebugContext& debug_;
    const bronze_fn_desc& desc_;
    std::unordered_map<uint32_t, uint32_t> local_;
    std::vector<bronze_inline_frame> frames_;
};

// runtime/profiler.h's describer: a MIR name to its JS function, looked up in
// every loaded program (a name is unique within one; the first match wins).
const bronze_fn_desc* describeMirName(std::string_view mirName) {
    std::lock_guard<std::mutex> lock(registryMutex());
    for (const auto& [module, image] : registry()) {
        if (const bronze_fn_desc* desc = image->descriptorOf(mirName)) return desc;
    }
    return nullptr;
}

// runtime/profiler.h's tier-1 rejection lookup: why the baseline tier
// rejected the program function of that MIR name, in the first loaded
// program that has one.
std::string tier1RejectionOf(std::string_view mirName) {
    std::lock_guard<std::mutex> lock(registryMutex());
    for (const auto& [module, image] : registry()) {
        if (!module->get_function(mirName)) continue;
        brass::runtime::MultiTierPipeline* pipeline = image->loadedPipeline();
        return pipeline ? pipeline->baseline_rejection(mirName) : std::string();
    }
    return {};
}

}  // namespace

std::unique_ptr<TieredProgramImage> TieredProgramImage::load(const brass::object::ObjectFile& image,
                                                             const brass::Module& module, const il::Module& il,
                                                             const std::string& entrySymbol,
                                                             brass::runtime::MultiTierPipeline& pipeline,
                                                             DiagnosticSink& diags) {
    support::PhaseTimer timer(support::timingsEnabled(), 6);
    std::unique_ptr<TieredProgramImage> self(new TieredProgramImage());
    self->module_ = &module;
    self->pipeline_ = &pipeline;
    self->data_ =std::make_unique<brass::codegen::JitExecutionEngine>(brass::Target::host());
    brass::codegen::JitExecutionEngine& data = *self->data_;
    registerBronzeJitSymbols(data);
    timer.mark("jit symbols");
    // The image's pointers to the program's functions (descriptors, source
    // entries) are the pipeline's function pointers: what a closure's code
    // pointer is in every tier.
    for (const brass::Function* fn : module.functions()) {
        if (!fn || !fn->has_body()) continue;
        data.register_external_symbol(fn->name(), pipeline.function_address(fn->name(), fn));
    }
    timer.mark("fn stubs");
    if (!data.load_object(image)) {
        diags.error(Span{}, "Failed to load the program's data image");
        return nullptr;
    }
    timer.mark("load object");

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

    // Descriptors resolve on first ask (descriptorOf).
    self->entrySymbol_ = entrySymbol;
    timer.mark("symbols");

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
    embed::setMirNameDescriber(&describeMirName);
    embed::setMirNameTier1Rejection(&tier1RejectionOf);
    return self;
}

void TieredProgramImage::detachPipeline() {
    std::lock_guard<std::mutex> lock(registryMutex());
    pipeline_ = nullptr;
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
    // Looked up on first ask rather than for all of a program's functions at
    // load: most never install code, and the load is on the page's critical
    // path. Asked from the mutator and from background compile threads.
    std::lock_guard<std::mutex> lock(descsMutex_);
    std::string key(mirName);
    auto it = descs_.find(key);
    if (it != descs_.end()) return it->second;
    const brass::Function* fn = module_ ? module_->get_function(mirName) : nullptr;
    const bronze_fn_desc* desc = nullptr;
    if (fn && fn->has_body() && data_) {
        const std::string unique = (key == entrySymbol_) ? "main" : key;
        desc = static_cast<const bronze_fn_desc*>(
            data_->get_symbol_address(codegen::moduleSymbolName(entrySymbol_, "__bronze_fn_desc_" + unique)));
    }
    descs_.emplace(std::move(key), desc);
    return desc;
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

void TieredProgramImage::expandInlined(const brass::DebugLoc& at, const brass::DebugContext& debug,
                                       const bronze_fn_desc& desc, std::vector<JsFrame>& out) const {
    brass::DebugLoc loc = at;
    uint32_t sid = loc.inlined_at_id;
    // Bounded by the scope count: a parent is always an earlier scope.
    for (size_t guard = 0; sid != 0 && guard <= debug.inlined_scope_count(); ++guard) {
        const brass::InlinedScope* scope = debug.get_inlined_scope(sid);
        if (!scope) break;
        if (const bronze_fn_desc* callee = descriptorOf(scope->callee_name)) {
            out.push_back(JsFrame{callee, translate(loc, *callee)});
        }
        loc = scope->callsite_loc;
        sid = scope->parent_inlined_at_id;
    }
    out.push_back(JsFrame{&desc, translate(loc, desc)});
}

void TieredProgramImage::onCodeInstalled(const brass::runtime::InstalledCode& code) {
    const bronze_fn_desc* desc = descriptorOf(code.name);
    if (!desc || !code.code || code.size == 0) return;
    // Code compiled from a copy (tier 2, OSR) names the copy's scopes, the
    // program's and those brass's inliner added; read only during this call.
    const brass::DebugContext& debug = code.debug_context ? *code.debug_context : module_->debug_context();
    std::lock_guard<std::mutex> lock(rangesMutex_);
    RangeRecord& r = ranges_.emplace_back();
    if (code.lines) {
        InstalledInlineFrames inlines(*this, debug, *desc);
        r.pcs.reserve(code.lines->size());
        for (const brass::DebugLineEntry& le : *code.lines) {
            brass::DebugLoc loc = le.loc;
            const uint32_t frame = inlines.frameOf(loc);
            // A position in an inlined body is the callee's own, its file
            // the callee's when the pc entry names none (find_code_site).
            const bronze_fn_desc& in = frame ? *inlines.frames()[frame - 1].desc : *desc;
            bronze_pc_entry e = translate(loc, in);
            e.pc_offset = le.code_offset;
            e.inline_frame = frame;
            r.pcs.push_back(e);
        }
        r.inlines = std::move(inlines.frames());
    }
    r.range.code_start = code.code;
    r.range.code_size = static_cast<uint32_t>(code.size);
    r.range.pc_count = static_cast<uint32_t>(r.pcs.size());
    r.range.desc = desc;
    r.range.pc_table = r.pcs.empty() ? nullptr : r.pcs.data();
    r.range.files = files_;
    r.range.file_count = fileCount_;
    r.range.inline_frames = r.inlines.empty() ? nullptr : r.inlines.data();
    r.range.inline_frame_count = static_cast<uint32_t>(r.inlines.size());
    bronze_register_code_ranges(&r.range, 1);
}

}  // namespace bronze
