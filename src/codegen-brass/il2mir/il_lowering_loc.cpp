// Source positions for the MIR an IL instruction lowers to, inline frames
// included (docs/il2mir.md section 10, "Stack traces: inline frames").

#include "il_lowering.h"

namespace il2mir {

using namespace brass;

// Every function's inline frames get their scopes here, in lower_module,
// before any body is built: the module's debug context is then fixed for the
// program's life. A lazily lowered module builds a body when brass first
// needs it (il_lowering_lazy.cpp), on the mutator or on a background
// compiler's thread, and builds a released one again; recording scopes there
// would append to the context while a stack walk, a code install or a tier-2
// copy (brass clone_function_module) reads it on another thread, and give a
// rebuilt body other ids than its first build's.
bool IlLowering::record_inline_scopes(const BronzeModuleAST& ast, Module& mod) {
    inline_scope_ids_by_fn_.assign(ast.functions.size(), {});
    for (size_t i = 0; i < ast.functions.size(); ++i) {
        const BronzeFunction& fn = ast.functions[i];
        if (fn.inline_frames.empty()) continue;
        std::vector<uint32_t>& ids = inline_scope_ids_by_fn_[i];
        ids.assign(fn.inline_frames.size(), 0);
        // A frame's parent is recorded before it (its scope id is the
        // child's parent id); the parent may come later in the table, hence
        // the walk up rather than one pass in order.
        std::vector<uint32_t> chain;
        for (uint32_t frame = 1; frame <= fn.inline_frames.size(); ++frame) {
            chain.clear();
            for (uint32_t f = frame; f != 0 && f <= ids.size() && ids[f - 1] == 0;
                 f = fn.inline_frames[f - 1].parent) {
                if (chain.size() > ids.size()) {
                    // A cycle: inlining only nests a frame in an earlier
                    // one, so this is a compiler bug.
                    has_error_ = true;
                    if (diag_) {
                        diag_->error(SourceLocation("", 0, 0), "il2mir: inline frame " + std::to_string(frame) +
                                                                   " of '" + fn.name + "' is its own ancestor");
                    }
                    return false;
                }
                chain.push_back(f);
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                const BronzeFunction::InlineFrame& f = fn.inline_frames[*it - 1];
                const uint32_t parent = (f.parent != 0 && f.parent <= ids.size()) ? ids[f.parent - 1] : 0;
                const uint32_t fileId = f.file < source_file_ids_.size() ? source_file_ids_[f.file] : current_file_id_;
                ids[*it - 1] =
                    mod.debug_context().record_inlined_scope(f.callee, DebugLoc(fileId, f.line, f.column, parent), parent);
            }
        }
    }
    return true;
}

// The scope lower_module recorded for the current function's inline frame.
uint32_t IlLowering::inline_scope_id(uint32_t frame) const {
    if (frame == 0 || current_fn_idx_ >= inline_scope_ids_by_fn_.size()) return 0;
    const std::vector<uint32_t>& ids = inline_scope_ids_by_fn_[current_fn_idx_];
    return frame <= ids.size() ? ids[frame - 1] : 0;
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
