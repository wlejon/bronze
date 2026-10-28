// Source positions for the MIR an IL instruction lowers to, inline frames
// included (docs/il2mir.md section 10, "Stack traces: inline frames").

#include "il_lowering.h"

namespace il2mir {

using namespace brass;

// An inline frame's scope, recorded on first use with its parent's: the
// callee by its function name, which is what the object writer finds its
// descriptor by, and the call's position as the scope's call site.
uint32_t IlLowering::inline_scope_id(uint32_t frame) {
    if (!current_fn_ast_ || frame == 0 || frame > inline_scope_ids_.size()) return 0;
    uint32_t& id = inline_scope_ids_[frame - 1];
    if (id != 0) return id;
    const BronzeFunction::InlineFrame& f = current_fn_ast_->inline_frames[frame - 1];
    const uint32_t parent = inline_scope_id(f.parent);
    const uint32_t fileId = f.file < source_file_ids_.size() ? source_file_ids_[f.file] : current_file_id_;
    Module* mod = current_mod_;
    if (!mod) return 0;
    id = mod->debug_context().record_inlined_scope(f.callee, DebugLoc(fileId, f.line, f.column, parent), parent);
    return id;
}

void IlLowering::apply_inst_loc(Builder& b, const BronzeInstruction& inst) {
    if (current_file_id_ != 0 && inst.line > 0) {
        // The instruction's own file: a merged top level holds lines of every
        // imported module, so the module's name is only the fallback for an
        // instruction that names none.
        const uint32_t fileId = inst.file < source_file_ids_.size() ? source_file_ids_[inst.file] : current_file_id_;
        b.set_current_loc(DebugLoc(fileId, inst.line, inst.column, inline_scope_id(inst.inline_frame)));
    } else {
        b.clear_current_loc();
    }
}

}  // namespace il2mir
