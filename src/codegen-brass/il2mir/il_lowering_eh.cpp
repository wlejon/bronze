// Exception edges: how a throw reaches its handler.
//
// A throw is a brass raise, not a value stored somewhere for the caller to
// test. A `throw` inside a protected region is a branch to the handler with
// the value; one outside leaves the function as a MIR `throw`. A call inside
// a protected region is an `invoke`: its unwind edge lands at a pad that
// hands the thrown value to the handler, whether the callee raised it
// natively (compiled JS) or as a C++ exception (the runtime, a host native).
// A call outside one is a plain call, and nothing after it looks at anything:
// an exception passes the frame by through its unwind data.
#include "il_lowering.h"

#include <string>

namespace il2mir {

void IlLowering::add_handler_params(const BronzeFunction& fn_ast, Builder& b,
                                    const std::unordered_map<uint32_t, BasicBlock*>& block_map) {
    handler_exc_.clear();
    protected_blocks_.clear();
    for (const auto& blk : fn_ast.blocks) {
        if (blk.handler_id == UINT32_MAX || handler_exc_.count(blk.handler_id)) continue;
        auto it = block_map.find(blk.handler_id);
        if (it == block_map.end()) continue;
        handler_exc_[blk.handler_id] = b.add_block_param(it->second, Type::tagged());
    }
}

Value* IlLowering::handler_exception(uint32_t block_id) const {
    auto it = handler_exc_.find(block_id);
    return it == handler_exc_.end() ? nullptr : it->second;
}

void IlLowering::note_protected_blocks(Function* fn, BasicBlock* il_bb, size_t first_new_block,
                                       uint32_t handler_id) {
    if (handler_id == UINT32_MAX || !handler_exc_.count(handler_id)) return;
    protected_blocks_.emplace_back(il_bb, handler_id);
    const auto& blocks = fn->blocks();
    for (size_t i = first_new_block; i < blocks.size(); ++i) {
        if (blocks[i] != il_bb) protected_blocks_.emplace_back(blocks[i], handler_id);
    }
}

void IlLowering::route_exception_edges(Builder& b, const std::unordered_map<uint32_t, BasicBlock*>& block_map) {
    // One pad per handler: every invoke of the region unwinds to it.
    std::unordered_map<uint32_t, BasicBlock*> pads;
    auto pad_for = [&](uint32_t handler_id) -> BasicBlock* {
        auto it = pads.find(handler_id);
        if (it != pads.end()) return it->second;
        BasicBlock* handler = block_map.at(handler_id);
        BasicBlock* pad = b.append_block("b" + std::to_string(handler_id) + "_pad");
        Value* thrown = b.build_landing_pad(Type::i64());
        b.build_br(handler, {ensure_type(thrown, Type::tagged(), b)});
        pads[handler_id] = pad;
        return pad;
    };

    uint32_t split_id = 0;
    std::vector<std::pair<BasicBlock*, uint32_t>> work = std::move(protected_blocks_);
    protected_blocks_.clear();
    while (!work.empty()) {
        auto [bb, handler_id] = work.back();
        work.pop_back();
        for (Instruction* inst = bb->head(); inst; inst = inst->next()) {
            if (inst->opcode() != Opcode::call) continue;
            // The instructions after the call continue in a block of their
            // own, the invoke's normal successor, which is protected by the
            // same handler and so is scanned in its turn.
            BasicBlock* pad = pad_for(handler_id);
            BasicBlock* cont = b.append_block(std::string(bb->name()) + "_c" + std::to_string(++split_id));
            while (Instruction* moved = inst->next()) {
                bb->remove_instruction(moved);
                cont->append_instruction(moved);
            }
            inst->set_opcode(Opcode::invoke);
            inst->set_normal_target(BranchTarget(cont, {}));
            inst->set_unwind_target(BranchTarget(pad, {}));
            work.emplace_back(cont, handler_id);
            break;
        }
    }
}

}  // namespace il2mir
