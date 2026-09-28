#include "il_ic_stub.h"
#include "il_abi.h"
#include "il_property.h"
#include <brass/mir/block.hpp>
#include <brass/mir/function.hpp>
#include <brass/mir/uses.hpp>
#include <vector>

namespace il2mir {

namespace {

bool is_zero(const Value* v) {
    const Instruction* def = v ? v->defining_instruction() : nullptr;
    return def && def->opcode() == Opcode::iconst_i64 && def->imm_i64() == 0;
}

// The block `bb` branches to when its test holds, for a block ending in
// `br_if` (the site's branch, or its armed form's placeholder).
BasicBlock* true_successor(BasicBlock* bb) {
    Instruction* term = bb->tail();
    if (!term || term->opcode() != Opcode::br_if) return nullptr;
    return term->true_target().block;
}

// A scan call as the lowering builds it: its address is read only by the
// site's `ne addr, 0` in its own block and by one `load.i64 addr, 0` in the
// block the site's hit branches to.
struct ScanSite {
    Instruction* call = nullptr;
    Instruction* test = nullptr;
    Instruction* load = nullptr;
};

bool match_scan(Instruction* call, ScanSite& site) {
    BasicBlock* bb = call->parent();
    BasicBlock* phit = true_successor(bb);
    if (!phit || phit == bb) return false;
    Value* addr = call->result();
    site = ScanSite{call, nullptr, nullptr};
    for (Instruction* inst : *bb) {
        if (inst == call || !uses_value(*inst, addr)) continue;
        if (site.test || inst->opcode() != Opcode::ne || inst->operand_count() != 2 || inst->operand(0) != addr ||
            !is_zero(inst->operand(1))) {
            return false;
        }
        site.test = inst;
    }
    for (Instruction* inst : *phit) {
        if (!uses_value(*inst, addr)) continue;
        if (site.load || inst->opcode() != Opcode::load || inst->operand_count() != 1 ||
            inst->operand(0) != addr || inst->offset() != 0 || inst->type() != Type::i64()) {
            return false;
        }
        site.load = inst;
    }
    return site.test && site.load;
}

} // namespace

size_t expand_ic_stubs(Function& fn) {
    std::vector<ScanSite> sites;
    for (BasicBlock* bb : fn.blocks()) {
        if (!bb) continue;
        for (Instruction* inst : *bb) {
            ScanSite site;
            if (inst && inst->opcode() == Opcode::call && inst->operand_count() == 3 &&
                inst->symbol() == kPolyScanHelper && match_scan(inst, site)) {
                sites.push_back(site);
            }
        }
    }
    if (sites.empty()) return 0;

    // The inline scan as the lowering always built it for a module that runs
    // in this process (il_ic_lowering.cpp). The test becomes the scan's hit
    // and the load the indexed slot load, its base computed where the load
    // is — past the hit, since it reads the receiver's overflow word.
    PropertyLoweringHelper pl;
    pl.set_in_process(true);
    Builder b(fn);
    for (const ScanSite& site : sites) {
        BasicBlock* bb = site.call->parent();
        BasicBlock* phit = site.load->parent();
        b.position_before(site.call);
        Value* hit = nullptr;
        Value* ptr = nullptr;
        Value* slot_word = nullptr;
        pl.expand_poly_scan(b, site.call, hit, ptr, slot_word);
        replace_uses_in(*bb, site.test->result(), hit);
        b.position_before(site.load);
        Value* v = pl.load_slot(b, ptr, slot_word);
        replace_uses_in(*phit, site.load->result(), v);
        phit->remove_instruction(site.load);
        bb->remove_instruction(site.test);
        bb->remove_instruction(site.call);
    }
    return sites.size();
}

} // namespace il2mir
