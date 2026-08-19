// The private-element instruction family: `#x` reads, writes, brand checks and
// the class-evaluation mint (ECMA-262 6.2.12).
//
// Every one of these is a helper call and nothing more. There is no inline
// fast path and deliberately no inline cache: a private access is a lookup in
// an object-keyed table (runtime/rt_private.cpp), which shares no machinery
// with the shape-and-slot property path an IC caches — a private name has no
// slot, and the table's own hash index is what the runtime rebuilds when the
// collector moves a key. Private state is a correctness feature first; where
// it turns up in a hot loop, the thing to cache is the table lookup, in the
// runtime, where the epoch that invalidates it lives.

#include <llvm/IR/Constants.h>

#include "abi/bronze_abi.h"
#include "codegen-llvm/llvm_func.h"

namespace bronze::codegen_llvm {

bool FunctionEmitter::emitPrivateOp(const il::Instruction& inst) {
    const AbiFns& abi = shared_.abi;
    const char* what = "Undefined operand in a private-element instruction";

    switch (inst.op) {
        case il::Op::PrivateNew: {
            if (!require(inst.result != il::kNoValue, "PrivateNew with no result")) return false;
            values_[inst.result] = builder_.CreateCall(abi.bronze_private_new, {});
            return true;
        }
        case il::Op::PrivateHas:
        case il::Op::PrivateGet: {
            const bool isGet = inst.op == il::Op::PrivateGet;
            if (!require(inst.operands.size() >= 2 && inst.result != il::kNoValue,
                         "Invalid operands for a private read")) {
                return false;
            }
            llvm::Value* table = operand(inst, 0, what);
            llvm::Value* obj = operand(inst, 1, what);
            if (!table || !obj) return false;
            llvm::Value* nameKey = emitKeyId(builder_, shared_.tables, inst.keyIndex);
            values_[inst.result] =
                isGet ? builder_.CreateCall(abi.bronze_private_get, {table, obj, nameKey})
                      : builder_.CreateCall(abi.bronze_private_has, {table, obj, nameKey});
            return true;
        }
        case il::Op::PrivateAdd:
        case il::Op::PrivateSet: {
            if (!require(inst.operands.size() >= 3, "Invalid operands for a private write")) {
                return false;
            }
            llvm::Value* table = operand(inst, 0, what);
            llvm::Value* obj = operand(inst, 1, what);
            llvm::Value* val = operand(inst, 2, what);
            if (!table || !obj || !val) return false;
            if (inst.op == il::Op::PrivateAdd) {
                builder_.CreateCall(abi.bronze_private_add, {table, obj, val});
            } else {
                builder_.CreateCall(abi.bronze_private_set,
                                    {table, obj, val,
                                     emitKeyId(builder_, shared_.tables, inst.keyIndex)});
            }
            return true;
        }
        // Always raises, exactly as immutable.assign does: the
        // exception check `il::canThrow` puts after it always fires, and the
        // `undefined` it returns exists only so the value id has a definition.
        case il::Op::PrivateMisuse: {
            llvm::Value* res = builder_.CreateCall(
                abi.bronze_private_misuse,
                {emitKeyId(builder_, shared_.tables, inst.keyIndex),
                 builder_.getInt32(static_cast<uint32_t>(inst.immI32))});
            if (inst.result != il::kNoValue) values_[inst.result] = res;
            return true;
        }
        default:
            return require(false, "emitPrivateOp on an instruction that is not a private one");
    }
}

}  // namespace bronze::codegen_llvm
