// 0/1 values, and the branch on a negated one.
//
// The IL writes `a !== b` as `cmp.eq (strict.eq a b), false` and `!x` of a
// Boolean the same way, so a branch on either is a branch on `eq c, 0`.
// When c is known to be 0 or 1 that is the branch on c with its targets
// swapped (il_lowering_inst.cpp, Branch), which leaves brass a compare to
// fuse in place of a compare, its zero extension and a test of that.

#include "il_lowering.h"

namespace il2mir {

namespace {

bool is_compare(Opcode op) {
    switch (op) {
        case Opcode::eq: case Opcode::ne:
        case Opcode::slt: case Opcode::sle: case Opcode::sgt: case Opcode::sge:
        case Opcode::ult: case Opcode::ule: case Opcode::ugt: case Opcode::uge:
            return true;
        default:
            return false;
    }
}

bool is_iconst(const Value* v, int64_t want) {
    const Instruction* d = v && v->is_instruction() ? v->defining_instruction() : nullptr;
    if (!d) return false;
    if (d->opcode() == Opcode::iconst_i32) return d->imm_i32() == want;
    if (d->opcode() == Opcode::iconst_i64) return d->imm_i64() == want;
    return false;
}

}  // namespace

bool IlLowering::is_zero_one(const Value* v) const {
    if (!v || v->type() != Type::i32()) return false;
    if (boolean_values_.count(v) || is_iconst(v, 0) || is_iconst(v, 1)) return true;
    const Instruction* d = v->is_instruction() ? v->defining_instruction() : nullptr;
    if (!d) return false;
    if (is_compare(d->opcode())) return true;
    return d->opcode() == Opcode::and_ && d->operand_count() == 2 &&
           (is_iconst(d->operand(0), 1) || is_iconst(d->operand(1), 1));
}

Value* IlLowering::negated_boolean(Value* cond) const {
    const Instruction* d = cond && cond->is_instruction() ? cond->defining_instruction() : nullptr;
    if (!d || d->opcode() != Opcode::eq || d->operand_count() != 2) return nullptr;
    Value* a = d->operand(0);
    Value* b = d->operand(1);
    if (is_iconst(b, 0) && is_zero_one(a)) return a;
    if (is_iconst(a, 0) && is_zero_one(b)) return b;
    return nullptr;
}

}  // namespace il2mir
