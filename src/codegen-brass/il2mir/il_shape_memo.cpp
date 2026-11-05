// THE SHAPE MEMO: a receiver's shape, read once and reused by the next guard
// on the same receiver.
//
// Every static-slot site (il_static_slot.cpp) and every inlined method's or
// accessor's guard (il_lowering_calls.cpp, `method.ic.is`, `accessor.ic.is`)
// asks the same first question
// of its receiver — is it a plain object, and what is its shape word — before
// it compares that word with its own cell or cache way. Lowered site by site,
// `bp.x ... bp.y ... bp.z` asks it three times of one SSA value: reload the
// receiver, untag it, select the payload or a stand-in, load the header and
// test it, load the shape. The answer is the receiver's KEYED SHAPE
// (PropertyLoweringHelper::keyed_shape: the shape word of a plain object, else
// a value no cell or way can hold), one i64, and the memo keeps it per
// receiver while it is still the answer. A later site then pays only its own
// compare: its cell (or way word) against the memo.
//
// WHEN THE ANSWER STAYS TRUE. An object's shape word changes only when its
// shape does — a property added, deleted or redefined, a prototype set, an
// extensibility change, a slot's representation widened — and each of those
// is an operation on that object. Its plainness is its heap kind, fixed when
// it is allocated. A moving collection changes where the object is, never its
// shape, and shapes are immortal and never move (runtime/static_shape.cpp),
// so the memo's word survives a collection as it is. So the memo stays true
// across an instruction that performs none of those operations on any
// object, and is forgotten whole at anything else: a call, a generic
// property access, an allocation, anything not named below. The list is
// written out (moves_no_shape) rather than inferred, so that an op added
// tomorrow forgets the memo until someone says it need not.
//
// A static site itself is the case that matters, and it is the case that is
// not pure: its hit is a load, or a store into a slot the shape already has
// (which moves no shape: the guard sent a value the slot's representation
// cannot take to the miss), but its MISS runs the ordinary access, which may
// run a getter or a setter, which may do anything to any object. So the site
// carries the memo across its own join: the hit edge passes every memo value
// on as it is, the miss edge recomputes each one after the access ran, and
// the join's parameters become the memo past the site (StaticMiss's memo
// hooks). The recomputation runs only on the miss.
//
// ACROSS BLOCKS the memo flows along an edge into a block with exactly one
// incoming edge, whose predecessor was lowered first: the predecessor's end
// state dominates the block's whole lowering, since the block is entered
// from nowhere else. A JOIN whose predecessors were all lowered first (a
// forward join, which reverse post-order lowers after every block that
// enters it) keeps a receiver every predecessor's end still holds: as that
// value when they all hold the same one, which then dominates the join, and
// else as a new i64 parameter of the join, each predecessor's edges into it
// passing the value its end holds (an `if (a || b) ...; else if (r.x ...)`
// chain joins once per test, and its tests read one receiver). A loop
// header (a back edge not lowered yet), a handler block (entered by a throw
// from anywhere in its region), and every block of a coroutine body (a
// suspension lets other code run before the resume) start with an empty
// memo.

#include "il_lowering.h"
#include <algorithm>
#include <cstdlib>
#include <stdexcept>

namespace il2mir {

namespace {

// More receivers than this alive at once is a function the memo's join
// parameters would cost more in than they save; the oldest is dropped.
constexpr size_t kMemoCap = 6;

// An instruction that performs no shape-changing operation on any object
// and runs no code that could: see the header.
bool moves_no_shape(const BronzeInstruction& inst) {
    if (is_pure_scalar_inst(inst)) return true;
    switch (inst.op) {
        case BronzeOp::MathUnary:     // an f64 kernel
        case BronzeOp::IsNumber:
        case BronzeOp::IsNullish:
        case BronzeOp::IsDenseArray:  // header loads
        case BronzeOp::IsTypedArray:
        case BronzeOp::IsShape:       // the static guard's loads
        case BronzeOp::LayoutFact:    // the same, and the epoch word
        // The claim's helper scans an Array's elements and rewrites an Int32
        // box as the same Number: an Array's element block, no shape.
        case BronzeOp::IsNumberArray:
        case BronzeOp::StrictEq:      // compares bits, or string contents
        case BronzeOp::EnvGet:        // environment slots, which are no shape
        case BronzeOp::EnvGetTdz:     // the same, or a throw that leaves
        case BronzeOp::ModuleEnvGet:
        case BronzeOp::KeepAlive:
        case BronzeOp::Jump:
        case BronzeOp::Branch:
            return true;
        // ToBoolean runs no user code.
        case BronzeOp::Unbox:
            return inst.result_type == BronzeType::Bool;
        default:
            return false;
    }
}

}  // namespace

void IlLowering::memo_begin_function(const BronzeFunction& fn_ast) {
    shape_memo_.clear();
    shape_memo_kept_ = false;
    shape_memo_at_end_.clear();
    memo_preds_.clear();
    memo_spans_.clear();
    memo_handler_blocks_.clear();
    shape_memo_off_ = fn_ast.is_coroutine_body();
    if (shape_memo_off_) return;
    for (const BronzeBlock& blk : fn_ast.blocks) {
        if (blk.handler_id != UINT32_MAX) memo_handler_blocks_.insert(blk.handler_id);
        if (blk.instructions.empty()) continue;
        const BronzeInstruction& term = blk.instructions.back();
        auto edge = [&](uint32_t succ) {
            auto& [count, preds] = memo_preds_[succ];
            ++count;
            if (std::find(preds.begin(), preds.end(), blk.id) == preds.end()) preds.push_back(blk.id);
        };
        if (term.op == BronzeOp::Jump) {
            edge(term.target.block_id);
        } else if (term.op == BronzeOp::Branch) {
            edge(term.target.block_id);
            edge(term.else_target.block_id);
        }
    }
}

void IlLowering::memo_begin_block(Builder& b, const BronzeBlock& blk, BasicBlock* bb) {
    shape_memo_.clear();
    shape_memo_kept_ = false;
    if (shape_memo_off_ || memo_handler_blocks_.count(blk.id) != 0) return;
    const auto pit = memo_preds_.find(blk.id);
    if (pit == memo_preds_.end()) return;
    if (pit->second.first == 1) {
        const auto eit = shape_memo_at_end_.find(pit->second.second.front());
        if (eit != shape_memo_at_end_.end()) shape_memo_ = eit->second;
        return;
    }
    memo_join(b, pit->second.second, bb);
}

void IlLowering::memo_join(Builder& b, const std::vector<uint32_t>& preds, BasicBlock* bb) {
    // Every predecessor lowered, with its MIR edges into the join found in
    // the blocks its own lowering made: a predecessor not lowered yet (a back
    // edge) or one whose edges are not all there to patch keeps the memo out.
    std::vector<const std::vector<std::pair<uint32_t, Value*>>*> ends;
    std::vector<std::vector<BranchTarget*>> edges;
    for (uint32_t pred : preds) {
        const auto eit = shape_memo_at_end_.find(pred);
        const auto sit = memo_spans_.find(pred);
        if (eit == shape_memo_at_end_.end() || sit == memo_spans_.end() || eit->second.empty()) return;
        ends.push_back(&eit->second);
        std::vector<BranchTarget*> into;
        for (BasicBlock* blk : sit->second) {
            Instruction* term = blk->terminator();
            if (term == nullptr) continue;
            if (term->branch_target().block == bb) into.push_back(&term->branch_target());
            if (term->true_target().block == bb) into.push_back(&term->true_target());
            if (term->false_target().block == bb) into.push_back(&term->false_target());
            for (SwitchCase& sc : term->switch_cases()) {
                if (sc.target.block == bb) into.push_back(&sc.target);
            }
        }
        if (into.empty()) return;
        edges.push_back(std::move(into));
    }
    for (const auto& [id, first] : *ends.front()) {
        std::vector<Value*> vals{first};
        bool everywhere = true;
        bool same = true;
        for (size_t p = 1; p < ends.size() && everywhere; ++p) {
            Value* v = nullptr;
            for (const auto& [pid, pv] : *ends[p]) {
                if (pid == id) v = pv;
            }
            everywhere = v != nullptr;
            same = same && v == first;
            vals.push_back(v);
        }
        if (!everywhere) continue;
        if (same) {
            shape_memo_.emplace_back(id, first);
            continue;
        }
        Value* param = b.add_block_param(bb, Type::i64());
        for (size_t p = 0; p < edges.size(); ++p) {
            for (BranchTarget* t : edges[p]) t->args.push_back(vals[p]);
        }
        shape_memo_.emplace_back(id, param);
    }
}

void IlLowering::memo_after_instruction(const BronzeInstruction& inst) {
    if (shape_memo_kept_) {
        shape_memo_kept_ = false;
        return;
    }
    if (!shape_memo_.empty() && !moves_no_shape(inst)) shape_memo_.clear();
}

void IlLowering::memo_end_block(const BronzeBlock& blk, BasicBlock* bb, Function* fn, size_t first_new) {
    if (shape_memo_off_) return;
    shape_memo_at_end_[blk.id] = shape_memo_;
    std::vector<BasicBlock*>& span = memo_spans_[blk.id];
    span.assign(1, bb);
    const auto& blocks = fn->blocks();
    for (size_t i = first_new; i < blocks.size(); ++i) {
        if (blocks[i] != bb) span.push_back(blocks[i]);
    }
}

Value* IlLowering::memo_lookup(uint32_t recv_id) const {
    for (const auto& [id, keyed] : shape_memo_) {
        if (id == recv_id) return keyed;
    }
    return nullptr;
}

void IlLowering::memo_add(uint32_t recv_id, Value* keyed) {
    if (shape_memo_off_) return;
    if (shape_memo_.size() >= kMemoCap) shape_memo_.erase(shape_memo_.begin());
    shape_memo_.emplace_back(recv_id, keyed);
}

Value* IlLowering::memo_keyed(Builder& b, uint32_t recv_id, Value* obj) {
    if (Value* keyed = memo_lookup(recv_id)) return keyed;
    Value* keyed = prop_lowering_.keyed_shape(b, obj);
    memo_add(recv_id, keyed);
    return keyed;
}

std::vector<Value*> IlLowering::memo_recompute(Builder& b, const std::unordered_map<uint32_t, Value*>& val_map) {
    std::vector<Value*> out;
    out.reserve(shape_memo_.size());
    for (const auto& entry : shape_memo_) {
        // The receiver is a tagged value the collector keeps current, so its
        // bits read here are the object's after whatever the miss ran.
        Value* obj = ensure_type(get_val_by_id(entry.first, b, val_map), Type::i64(), b);
        out.push_back(prop_lowering_.keyed_shape(b, obj));
    }
    return out;
}

std::optional<MemoCarry> IlLowering::memo_carry(uint32_t recv_id, Value* obj,
                                                const std::unordered_map<uint32_t, Value*>& val_map) {
    if (shape_memo_off_) return std::nullopt;
    MemoCarry carry;
    carry.keyed = [this, recv_id, obj](Builder& bb) { return memo_keyed(bb, recv_id, obj); };
    carry.hit = [this](Builder&) {
        std::vector<Value*> out;
        for (const auto& entry : shape_memo_) out.push_back(entry.second);
        return out;
    };
    carry.miss = [this, &val_map](Builder& bb) { return memo_recompute(bb, val_map); };
    carry.merged = [this](const std::vector<Value*>& params) {
        if (params.size() != shape_memo_.size()) {
            throw std::logic_error("il2mir: an inline-cache read's join lost track of the shape memo");
        }
        for (size_t i = 0; i < params.size(); ++i) shape_memo_[i].second = params[i];
        memo_keep();
    };
    return carry;
}

void IlLowering::memo_attach(Builder& b, StaticMiss& miss, uint32_t recv_id, Value* obj,
                             const std::unordered_map<uint32_t, Value*>& val_map) {
    // A receiver the memo holds is compared as it stands; one it does not
    // is tested by the site on its own plain-only path (plain_shape_or_miss),
    // which hands back the shape word, remembered as its keyed shape where
    // the site's hit is.
    (void)obj;
    (void)b;
    miss.keyed = memo_lookup(recv_id);
    if (shape_memo_off_) return;
    if (!miss.keyed) miss.memo_fresh = [this, recv_id](Value* shape) { memo_add(recv_id, shape); };
    miss.memo_hit = [this](Builder&) {
        std::vector<Value*> out;
        for (const auto& entry : shape_memo_) out.push_back(entry.second);
        return out;
    };
    miss.memo_miss = [this, &val_map](Builder& bb) { return memo_recompute(bb, val_map); };
    miss.memo_merged = [this](const std::vector<Value*>& params) {
        if (params.size() != shape_memo_.size()) {
            throw std::logic_error("il2mir: a static site's join lost track of the shape memo");
        }
        for (size_t i = 0; i < params.size(); ++i) shape_memo_[i].second = params[i];
        memo_keep();
    };
}

}  // namespace il2mir
