// Source positions for the MIR an IL instruction lowers to.

#include <mutex>

#include "il_lowering.h"

namespace il2mir {

using namespace brass;

void IlLowering::apply_inst_loc(Builder& b, const BronzeInstruction& inst) {
    if (current_file_id_ != 0 && inst.line > 0) {
        // The instruction's own file: a merged top level holds lines of every
        // imported module, so the module's name is only the fallback for an
        // instruction that names none.
        const uint32_t fileId = inst.file < source_file_ids_.size() ? source_file_ids_[inst.file] : current_file_id_;
        const uint32_t inlinedAt = inst.inline_scope != 0 && inst.inline_scope <= inline_scope_ids_.size()
                                       ? inline_scope_ids_[inst.inline_scope - 1]
                                       : 0;
        b.set_current_loc(DebugLoc(fileId, inst.line, inst.column, inlinedAt));
    } else {
        b.clear_current_loc();
    }
}

// Each scope is a brass inlined scope: the callee's function, which the
// code-range writer turns into a frame of its own (brass_backend_sections.cpp
// InlineFrameTable), at the call's position in the scope around it. The
// module's debug context is shared by the threads lowering its functions.
void IlLowering::record_inline_scopes(const BronzeFunction& fn_ast, Module& mod) {
    static std::mutex scopes_mutex;
    inline_scope_ids_.clear();
    if (fn_ast.inline_scopes.empty()) return;
    std::lock_guard<std::mutex> lock(scopes_mutex);
    for (const BronzeFunction::InlineScope& scope : fn_ast.inline_scopes) {
        const uint32_t parent =
            scope.parent != 0 && scope.parent <= inline_scope_ids_.size() ? inline_scope_ids_[scope.parent - 1] : 0;
        const uint32_t fileId = scope.file < source_file_ids_.size() ? source_file_ids_[scope.file] : current_file_id_;
        inline_scope_ids_.push_back(mod.debug_context().record_inlined_scope(
            scope.callee, DebugLoc(fileId, scope.line, scope.column, parent), parent));
    }
}

}  // namespace il2mir
