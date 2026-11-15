// Source positions for the MIR an IL instruction lowers to.

#include "il_lowering.h"

namespace il2mir {

using namespace brass;

void IlLowering::apply_inst_loc(Builder& b, const BronzeInstruction& inst) {
    if (current_file_id_ != 0 && inst.line > 0) {
        // The instruction's own file: a merged top level holds lines of every
        // imported module, so the module's name is only the fallback for an
        // instruction that names none.
        const uint32_t fileId = inst.file < source_file_ids_.size() ? source_file_ids_[inst.file] : current_file_id_;
        b.set_current_loc(DebugLoc(fileId, inst.line, inst.column, 0));
    } else {
        b.clear_current_loc();
    }
}

}  // namespace il2mir
