// The IL operations that become a runtime helper call, an inlined property
// access, or a constant. Arithmetic is llvm_arith.cpp; control flow is the
// terminator half of llvm_func.cpp.

#include <string>

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>

#include "abi/bronze_abi.h"
#include "codegen-llvm/llvm_cache.h"
#include "codegen-llvm/llvm_call.h"
#include "codegen-llvm/llvm_construct.h"
#include "codegen-llvm/llvm_elem.h"
#include "codegen-llvm/llvm_env.h"
#include "codegen-llvm/llvm_func.h"
#include "codegen-llvm/llvm_math.h"
#include "codegen-llvm/llvm_prop.h"

namespace bronze::codegen_llvm {

bool FunctionEmitter::emitRuntimeOp(const il::Instruction& inst) {
    const AbiFns& abi = shared_.abi;

    // The shapes most of these share: N operands in, one helper call, result
    // into the value table.
    auto needs = [&](size_t operandCount, bool needsResult, const char* what) {
        return require(inst.operands.size() >= operandCount &&
                           (!needsResult || inst.result != il::kNoValue),
                       what);
    };
    auto callWith = [&](llvm::Function* fn, std::initializer_list<llvm::Value*> args) {
        llvm::Value* res = builder_.CreateCall(fn, std::vector<llvm::Value*>(args));
        if (inst.result != il::kNoValue) values_[inst.result] = res;
    };

    switch (inst.op) {
        case il::Op::ConstF64:
            if (inst.result != il::kNoValue) {
                values_[inst.result] = llvm::ConstantFP::get(builder_.getDoubleTy(), inst.immF64);
            }
            return true;
        case il::Op::ConstI32:
            if (inst.result != il::kNoValue) values_[inst.result] = builder_.getInt32(inst.immI32);
            return true;
        case il::Op::ConstBool:
            if (inst.result != il::kNoValue) {
                values_[inst.result] = builder_.getInt1(inst.immI32 != 0);
            }
            return true;
        case il::Op::ConstUndefined:
            if (inst.result != il::kNoValue) {
                values_[inst.result] = builder_.getInt64(BRONZE_ABI_UNDEFINED_BITS);
            }
            return true;
        case il::Op::ConstNull:
            if (inst.result != il::kNoValue) {
                values_[inst.result] = builder_.getInt64(BRONZE_ABI_NULL_BITS);
            }
            return true;

        case il::Op::ConstBigInt:
            // A call and not a constant: the value has no width, so there is
            // no immediate to fold it into, and the runtime parses the source
            // text the key index names. A fresh object per evaluation is
            // unobservable — a BigInt is immutable and `===` compares value.
            if (inst.result != il::kNoValue) {
                values_[inst.result] = builder_.CreateCall(abi.bronze_bigint_literal,
                                                           {builder_.getInt32(inst.keyIndex)});
            }
            return true;

        case il::Op::ExcTake: {
            // The first instruction of every handler block. Reading and
            // CLEARING together is what lets a `finally` run its body with
            // nothing pending and then decide whether to re-raise — two
            // instructions here, no helper call, because the cell is an
            // ordinary global on both sides.
            if (inst.result == il::kNoValue) return true;
            values_[inst.result] =
                builder_.CreateLoad(i64Ty_, shared_.globals.bronze_exception_cell);
            builder_.CreateStore(builder_.getInt64(BRONZE_ABI_NO_EXCEPTION_BITS),
                                 shared_.globals.bronze_exception_cell);
            return true;
        }

        case il::Op::Box: {
            if (inst.result == il::kNoValue) return true;
            // A Str box names a registered key rather than boxing an operand:
            // the string itself is a compile-time constant.
            if (inst.boxType == il::Type::Str) {
                values_[inst.result] =
                    builder_.CreateCall(abi.bronze_box_str_key, {builder_.getInt32(inst.keyIndex)});
                return true;
            }
            if (!needs(1, true, "Invalid operands for Box")) return false;
            llvm::Value* src = operand(inst, 0, "Undefined value in Box instruction");
            if (!src) return false;
            if (inst.boxType == il::Type::F64 || src->getType()->isDoubleTy()) {
                llvm::Value* isNan = builder_.CreateFCmpUNO(src, src);
                llvm::Value* bitcast = builder_.CreateBitCast(src, builder_.getInt64Ty());
                values_[inst.result] = builder_.CreateSelect(
                    isNan, builder_.getInt64(BRONZE_ABI_CANONICAL_NAN_BITS), bitcast);
                return true;
            }
            if (inst.boxType == il::Type::Bool || src->getType()->isIntegerTy(1)) {
                llvm::Value* isTrue = builder_.CreateIsNotNull(src);
                llvm::Value* tagShifted =
                    builder_.getInt64(static_cast<uint64_t>(BRONZE_ABI_TAG_BOOL) << BRONZE_ABI_VALUE_TAG_SHIFT);
                llvm::Value* zext = builder_.CreateZExt(isTrue, builder_.getInt64Ty());
                values_[inst.result] = builder_.CreateOr(tagShifted, zext);
                return true;
            }
            if (inst.boxType == il::Type::I32 || src->getType()->isIntegerTy(32)) {
                llvm::Value* tagShifted =
                    builder_.getInt64(static_cast<uint64_t>(BRONZE_ABI_TAG_INT32) << BRONZE_ABI_VALUE_TAG_SHIFT);
                llvm::Value* zext = builder_.CreateZExt(src, builder_.getInt64Ty());
                values_[inst.result] = builder_.CreateOr(tagShifted, zext);
                return true;
            }
            values_[inst.result] = builder_.CreateCall(abi.bronze_box_f64, {src});
            return true;
        }
        case il::Op::Unbox: {
            if (!needs(1, true, "Invalid operands for Unbox")) return false;
            llvm::Value* src = operand(inst, 0, "Undefined value in Unbox instruction");
            if (!src) return false;
            if (inst.type == il::Type::I32) {
                llvm::Type* dblTy = builder_.getDoubleTy();
                llvm::Value* isNum = builder_.CreateICmpULE(
                    src, builder_.getInt64(BRONZE_ABI_NUMBER_MAX_BITS), "unbox.i32.isnum");
                llvm::Value* fastDouble = builder_.CreateBitCast(src, dblTy);
                // "Is a number" is not enough to license the conversion, and
                // this is the same defect emitElemGuards' fptoui had: `fptosi`
                // of a double outside the destination range is POISON — not a
                // wrong number, a value the optimizer may assume never occurs —
                // and it flows straight into the phi below. ToInt32 also does
                // not TRUNCATE out of range, it wraps modulo 2^32, so an
                // in-range test is what the language wants anyway; everything
                // else goes to the helper, which owns the wrap. NaN fails both
                // ordered compares and takes the same road.
                llvm::Value* ge = builder_.CreateFCmpOGE(
                    fastDouble, llvm::ConstantFP::get(dblTy, -2147483648.0));
                llvm::Value* lt = builder_.CreateFCmpOLT(
                    fastDouble, llvm::ConstantFP::get(dblTy, 2147483648.0));
                llvm::Value* inRange = builder_.CreateAnd(ge, lt);
                // The select is what makes the operand safe on EVERY path
                // rather than merely on the taken one: both live in this basic
                // block, and ordering the checks does not stop the optimizer
                // from folding the conversion first.
                llvm::Value* safeDouble = builder_.CreateSelect(
                    inRange, fastDouble, llvm::ConstantFP::get(dblTy, 0.0));
                llvm::Value* fastI32 =
                    builder_.CreateFPToSI(safeDouble, builder_.getInt32Ty(), "unbox.i32.val");
                isNum = builder_.CreateAnd(isNum, inRange);

                llvm::LLVMContext& ctx = builder_.getContext();
                llvm::Function* fn = builder_.GetInsertBlock()->getParent();
                llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "unbox.i32.slow", fn);
                llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "unbox.i32.done", fn);
                llvm::BasicBlock* curBb = builder_.GetInsertBlock();

                builder_.CreateCondBr(isNum, doneBb, slowBb);

                builder_.SetInsertPoint(slowBb);
                llvm::Value* slowVal = builder_.CreateCall(abi.bronze_unbox_i32, {src});
                llvm::BasicBlock* slowEndBb = builder_.GetInsertBlock();
                builder_.CreateBr(doneBb);

                builder_.SetInsertPoint(doneBb);
                llvm::PHINode* phi = builder_.CreatePHI(builder_.getInt32Ty(), 2, "unbox.i32.res");
                phi->addIncoming(fastI32, curBb);
                phi->addIncoming(slowVal, slowEndBb);
                values_[inst.result] = phi;
                return true;
            }
            if (inst.type == il::Type::Bool) {
                llvm::Value* isNum = builder_.CreateICmpULE(
                    src, builder_.getInt64(BRONZE_ABI_NUMBER_MAX_BITS), "unbox.bool.isnum");
                llvm::Value* fastDouble = builder_.CreateBitCast(src, builder_.getDoubleTy());
                llvm::Value* fastTruthy = builder_.CreateFCmpONE(fastDouble, llvm::ConstantFP::get(builder_.getDoubleTy(), 0.0), "unbox.bool.val");

                llvm::LLVMContext& ctx = builder_.getContext();
                llvm::Function* fn = builder_.GetInsertBlock()->getParent();
                llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "unbox.bool.slow", fn);
                llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "unbox.bool.done", fn);
                llvm::BasicBlock* curBb = builder_.GetInsertBlock();

                builder_.CreateCondBr(isNum, doneBb, slowBb);

                builder_.SetInsertPoint(slowBb);
                llvm::Value* slowVal = builder_.CreateCall(abi.bronze_unbox_bool, {src});
                llvm::BasicBlock* slowEndBb = builder_.GetInsertBlock();
                builder_.CreateBr(doneBb);

                builder_.SetInsertPoint(doneBb);
                llvm::PHINode* phi = builder_.CreatePHI(builder_.getInt1Ty(), 2, "unbox.bool.res");
                phi->addIncoming(fastTruthy, curBb);
                phi->addIncoming(slowVal, slowEndBb);
                values_[inst.result] = phi;
                return true;
            }
            if (inst.type == il::Type::F64) {
                llvm::Value* isNum = builder_.CreateICmpULE(
                    src, builder_.getInt64(BRONZE_ABI_NUMBER_MAX_BITS), "unbox.isnum");
                llvm::Value* fastDouble = builder_.CreateBitCast(src, builder_.getDoubleTy());

                llvm::LLVMContext& ctx = builder_.getContext();
                llvm::Function* fn = builder_.GetInsertBlock()->getParent();
                llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "unbox.slow", fn);
                llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "unbox.done", fn);
                llvm::BasicBlock* curBb = builder_.GetInsertBlock();

                builder_.CreateCondBr(isNum, doneBb, slowBb);

                builder_.SetInsertPoint(slowBb);
                llvm::Value* slowVal = builder_.CreateCall(abi.bronze_unbox_f64, {src});
                llvm::BasicBlock* slowEndBb = builder_.GetInsertBlock();
                builder_.CreateBr(doneBb);

                builder_.SetInsertPoint(doneBb);
                llvm::PHINode* phi = builder_.CreatePHI(builder_.getDoubleTy(), 2, "unbox.val");
                phi->addIncoming(fastDouble, curBb);
                phi->addIncoming(slowVal, slowEndBb);
                values_[inst.result] = phi;
                return true;
            }
            values_[inst.result] = builder_.CreateCall(abi.bronze_unbox_f64, {src});
            return true;
        }

        case il::Op::CreateObject:
            if (inst.result != il::kNoValue) {
                values_[inst.result] = emitCreateObjectInline(builder_, abi, shared_.globals);
            }
            return true;
        case il::Op::CreateGeneratorObject: {
            // The operand is the RESUME FUNCTION: a generator object is the
            // body it can be re-entered at, and nothing else.
            if (!needs(1, false, "Invalid operands for CreateGeneratorObject")) return false;
            llvm::Value* body =
                operand(inst, 0, "Undefined operand in CreateGeneratorObject instruction");
            if (!body) return false;
            callWith(abi.bronze_create_generator_object, {body});
            return true;
        }
        case il::Op::CreateAsyncGeneratorObject: {
            if (!needs(1, false, "Invalid operands for CreateAsyncGeneratorObject")) return false;
            llvm::Value* body =
                operand(inst, 0, "Undefined operand in CreateAsyncGeneratorObject instruction");
            if (!body) return false;
            callWith(abi.bronze_create_async_generator_object, {body});
            return true;
        }
        case il::Op::DynamicImport: {
            if (!needs(1, false, "Invalid operands for DynamicImport")) return false;
            llvm::Value* spec = operand(inst, 0, "Undefined operand in DynamicImport instruction");
            if (!spec) return false;
            callWith(abi.bronze_dynamic_import, {spec});
            return true;
        }
        case il::Op::CreateAsyncMachine: {
            // The operand is the resume closure, exactly as it is for the
            // generator object above — one machine body, two drivers.
            if (!needs(1, false, "Invalid operands for CreateAsyncMachine")) return false;
            llvm::Value* body =
                operand(inst, 0, "Undefined operand in CreateAsyncMachine instruction");
            if (!body) return false;
            callWith(abi.bronze_async_machine, {body});
            return true;
        }
        case il::Op::AsyncStart: {
            if (!needs(1, false, "Invalid operands for AsyncStart")) return false;
            llvm::Value* machine =
                operand(inst, 0, "Undefined operand in AsyncStart instruction");
            if (!machine) return false;
            callWith(abi.bronze_async_start, {machine});
            return true;
        }
        case il::Op::AsyncAwait: {
            if (!needs(2, false, "Invalid operands for AsyncAwait")) return false;
            llvm::Value* machine =
                operand(inst, 0, "Undefined operand in AsyncAwait instruction");
            llvm::Value* awaited =
                operand(inst, 1, "Undefined operand in AsyncAwait instruction");
            if (!machine || !awaited) return false;
            builder_.CreateCall(abi.bronze_async_await, {machine, awaited});
            return true;
        }
        case il::Op::ModuleNamespace: {
            if (!needs(1, false, "Invalid operands for ModuleNamespace")) return false;
            llvm::Value* src = operand(inst, 0, "Undefined operand in ModuleNamespace instruction");
            if (!src) return false;
            callWith(abi.bronze_module_namespace, {src});
            return true;
        }
        case il::Op::CreateArray:
            if (inst.result != il::kNoValue) {
                callWith(abi.bronze_create_array, {builder_.getInt32(inst.immI32)});
            }
            return true;
        case il::Op::ObjectKeys: {
            if (!needs(1, false, "Invalid operands for ObjectKeys")) return false;
            llvm::Value* target = operand(inst, 0, "Undefined operand in ObjectKeys instruction");
            if (!target) return false;
            callWith(abi.bronze_object_keys, {target});
            return true;
        }
        case il::Op::ForInKeys: {
            if (!needs(1, false, "Invalid operands for ForInKeys")) return false;
            llvm::Value* target = operand(inst, 0, "Undefined operand in ForInKeys instruction");
            if (!target) return false;
            callWith(abi.bronze_for_in_keys, {target});
            return true;
        }
        case il::Op::MethodDef: {
            if (!needs(2, false, "Invalid operands for MethodDef")) return false;
            const char* what = "Undefined operand in MethodDef instruction";
            llvm::Value* target = operand(inst, 0, what);
            llvm::Value* value = operand(inst, 1, what);
            if (!target || !value) return false;
            callWith(abi.bronze_method_def,
                     {target, builder_.getInt32(inst.keyIndex), value});
            return true;
        }
        case il::Op::MethodDefComputed: {
            if (!needs(3, false, "Invalid operands for MethodDefComputed")) return false;
            const char* what = "Undefined operand in MethodDefComputed instruction";
            llvm::Value* target = operand(inst, 0, what);
            llvm::Value* key = operand(inst, 1, what);
            llvm::Value* value = operand(inst, 2, what);
            if (!target || !key || !value) return false;
            callWith(abi.bronze_method_def_computed, {target, key, value});
            return true;
        }
        case il::Op::AccessorDef: {
            if (!needs(3, false, "Invalid operands for AccessorDef")) return false;
            const char* what = "Undefined operand in AccessorDef instruction";
            llvm::Value* target = operand(inst, 0, what);
            llvm::Value* getter = operand(inst, 1, what);
            llvm::Value* setter = operand(inst, 2, what);
            if (!target || !getter || !setter) return false;
            callWith(abi.bronze_accessor_def,
                     {target, builder_.getInt32(inst.keyIndex), getter, setter,
                      builder_.getInt1(inst.immI32 != 0)});
            return true;
        }
        case il::Op::AccessorDefComputed: {
            if (!needs(4, false, "Invalid operands for AccessorDefComputed")) return false;
            const char* what = "Undefined operand in AccessorDefComputed instruction";
            llvm::Value* target = operand(inst, 0, what);
            llvm::Value* key = operand(inst, 1, what);
            llvm::Value* getter = operand(inst, 2, what);
            llvm::Value* setter = operand(inst, 3, what);
            if (!target || !key || !getter || !setter) return false;
            builder_.CreateCall(abi.bronze_accessor_def_computed,
                                {target, key, getter, setter,
                                 builder_.getInt1(inst.immI32 != 0)});
            return true;
        }
        case il::Op::GetNewTarget:
            if (inst.result != il::kNoValue) {
                callWith(abi.bronze_get_new_target, {});
            }
            return true;
        // `import.meta` carries its module's URL as a key-constant index, the
        // same currency a Str box uses, so the call takes an index and no
        // operand.
        case il::Op::ImportMeta:
            if (inst.result != il::kNoValue) {
                callWith(abi.bronze_import_meta, {builder_.getInt32(inst.keyIndex)});
            }
            return true;
        case il::Op::SuperGet: {
            if (!needs(2, false, "Invalid operands for SuperGet")) return false;
            const char* what = "Undefined operand in SuperGet instruction";
            llvm::Value* proto = operand(inst, 0, what);
            llvm::Value* thisArg = operand(inst, 1, what);
            if (!proto || !thisArg) return false;
            callWith(abi.bronze_super_get,
                     {proto, builder_.getInt32(inst.keyIndex), thisArg});
            return true;
        }
        case il::Op::SuperSet: {
            if (!needs(3, false, "Invalid operands for SuperSet")) return false;
            const char* what = "Undefined operand in SuperSet instruction";
            llvm::Value* proto = operand(inst, 0, what);
            llvm::Value* thisArg = operand(inst, 1, what);
            llvm::Value* val = operand(inst, 2, what);
            if (!proto || !thisArg || !val) return false;
            builder_.CreateCall(abi.bronze_super_set,
                                {proto, builder_.getInt32(inst.keyIndex), thisArg, val});
            return true;
        }
        case il::Op::PropDelete: {
            if (!needs(1, false, "Invalid operands for PropDelete")) return false;
            llvm::Value* target = operand(inst, 0, "Undefined operand in PropDelete instruction");
            if (!target) return false;
            callWith(abi.bronze_prop_delete,
                     {target, builder_.getInt32(inst.keyIndex), builder_.getInt1(inst.immI32 != 0)});
            return true;
        }
        case il::Op::ElemDelete: {
            if (!needs(2, false, "Invalid operands for ElemDelete")) return false;
            const char* what = "Undefined operand in ElemDelete instruction";
            llvm::Value* target = operand(inst, 0, what);
            llvm::Value* index = operand(inst, 1, what);
            if (!target || !index) return false;
            callWith(abi.bronze_elem_delete, {target, index, builder_.getInt1(inst.immI32 != 0)});
            return true;
        }
        case il::Op::GlobalGet:
            if (inst.result != il::kNoValue) {
                // The helper's committed fast path — a cached, non-undefined
                // cell — read inline off the published rooted table; the
                // helper keeps every fill and every fallthrough.
                values_[inst.result] =
                    emitGlobalGetCached(builder_, abi, shared_.globals, inst.keyIndex);
            }
            return true;
        // The result is `undefined` and nothing reads it: the exception check
        // `il::canThrow` puts after this instruction always fires, so control
        // never reaches a use. It is still materialized, because a value id
        // with no definition is exactly what the verifier exists to catch.
        case il::Op::RefError:
            callWith(abi.bronze_reference_error, {builder_.getInt32(inst.keyIndex)});
            return true;
        // Same shape as ref.error above and for the same reason: the helper
        // always raises, the exception check after it always fires, and the
        // `undefined` it returns is materialized only so the verifier sees a
        // definition for the value id.
        case il::Op::ImmutableAssign:
            callWith(abi.bronze_immutable_assign, {});
            return true;
        case il::Op::ClassExtend: {
            if (!needs(2, false, "Invalid operands for ClassExtend")) return false;
            llvm::Value* derived = operand(inst, 0, "Undefined operand in ClassExtend instruction");
            llvm::Value* base = operand(inst, 1, "Undefined operand in ClassExtend instruction");
            if (!derived || !base) return false;
            builder_.CreateCall(abi.bronze_class_extends, {derived, base});
            return true;
        }

        case il::Op::IterOpen:
        case il::Op::AsyncIterOpen:
        case il::Op::AsyncIterNext:
        case il::Op::IterStep:
        case il::Op::IterValue:
        case il::Op::IterRest: {
            if (!needs(1, true, "Invalid operands for an iteration instruction")) return false;
            llvm::Value* rec = operand(inst, 0, "Undefined value in an iteration instruction");
            if (!rec) return false;
            switch (inst.op) {
                case il::Op::IterOpen: callWith(abi.bronze_iter_open, {rec}); break;
                case il::Op::AsyncIterOpen: callWith(abi.bronze_async_iter_open, {rec}); break;
                case il::Op::AsyncIterNext: callWith(abi.bronze_async_iter_next, {rec}); break;
                case il::Op::IterStep: callWith(abi.bronze_iter_step, {rec}); break;
                case il::Op::IterValue: callWith(abi.bronze_iter_value, {rec}); break;
                default: callWith(abi.bronze_iter_rest, {rec}); break;
            }
            return true;
        }

        case il::Op::IterDelegate: {
            if (!needs(3, true, "Invalid operands for IterDelegate")) return false;
            const char* what = "Undefined operand in an IterDelegate instruction";
            llvm::Value* rec = operand(inst, 0, what);
            llvm::Value* mode = operand(inst, 1, what);
            llvm::Value* sent = operand(inst, 2, what);
            if (!rec || !mode || !sent) return false;
            callWith(abi.bronze_iter_delegate, {rec, mode, sent});
            return true;
        }

        case il::Op::IterClose:
        case il::Op::AsyncIterClose: {
            if (!needs(1, false, "Invalid operands for IterClose")) return false;
            llvm::Value* rec = operand(inst, 0, "Undefined record in IterClose instruction");
            if (!rec) return false;
            llvm::Function* closeFn = inst.op == il::Op::AsyncIterClose
                                          ? abi.bronze_async_iter_close
                                          : abi.bronze_iter_close;
            builder_.CreateCall(closeFn, {rec, builder_.getInt1(inst.immI32 != 0)});
            return true;
        }
        case il::Op::PatternCheck: {
            if (!needs(1, true, "Invalid operands for PatternCheck")) return false;
            llvm::Value* src = operand(inst, 0, "Undefined source in PatternCheck instruction");
            if (!src) return false;
            callWith(abi.bronze_pattern_check,
                     {src, builder_.getInt32(static_cast<uint32_t>(inst.immI32))});
            return true;
        }
        case il::Op::ArrayAppend:
        case il::Op::ArraySpread:
        case il::Op::ObjectSpread: {
            if (!needs(2, false, "Invalid operands for a container-building instruction")) {
                return false;
            }
            const char* what = "Undefined operand in a container-building instruction";
            llvm::Value* container = operand(inst, 0, what);
            llvm::Value* value = operand(inst, 1, what);
            if (!container || !value) return false;
            llvm::Function* fn = inst.op == il::Op::ArrayAppend  ? abi.bronze_array_append
                                 : inst.op == il::Op::ArraySpread ? abi.bronze_array_spread
                                                                  : abi.bronze_object_spread;
            builder_.CreateCall(fn, {container, value});
            return true;
        }
        case il::Op::ArrayAppendHole: {
            if (!needs(1, false, "Invalid operands for ArrayAppendHole")) return false;
            llvm::Value* container = operand(inst, 0, "Undefined operand in ArrayAppendHole");
            if (!container) return false;
            builder_.CreateCall(abi.bronze_array_append_hole, {container});
            return true;
        }
        case il::Op::TemplateObject: {
            if (!needs(2, true, "Invalid operands for TemplateObject")) return false;
            const char* what = "Undefined operand in TemplateObject instruction";
            llvm::Value* cooked = operand(inst, 0, what);
            llvm::Value* raw = operand(inst, 1, what);
            if (!cooked || !raw) return false;
            callWith(abi.bronze_template_object, {cooked, raw});
            return true;
        }
        case il::Op::ObjectRest: {
            if (!needs(2, true, "Invalid operands for ObjectRest")) return false;
            const char* what = "Undefined operand in ObjectRest instruction";
            llvm::Value* src = operand(inst, 0, what);
            llvm::Value* excluded = operand(inst, 1, what);
            if (!src || !excluded) return false;
            callWith(abi.bronze_object_rest, {src, excluded});
            return true;
        }
        case il::Op::DynamicCallSpread: {
            if (!needs(3, true, "Invalid operands for DynamicCallSpread")) return false;
            const char* what = "Undefined operand in DynamicCallSpread instruction";
            llvm::Value* callee = operand(inst, 0, what);
            llvm::Value* thisVal = operand(inst, 1, what);
            llvm::Value* args = operand(inst, 2, what);
            if (!callee || !thisVal || !args) return false;
            callWith(abi.bronze_dynamic_call_spread, {callee, thisVal, args});
            return true;
        }
        case il::Op::SuperCall: {
            if (!needs(2, false, "Invalid operands for SuperCall")) return false;
            llvm::Value* base =
                operand(inst, 0, "Undefined base in SuperCall instruction");
            llvm::Value* thisVal =
                operand(inst, 1, "Undefined this in SuperCall instruction");
            if (!base || !thisVal) return false;
            uint32_t argc = static_cast<uint32_t>(inst.operands.size() - 2);
            bool ok = false;
            llvm::Value* argv = emitArgv(inst, 2, argc, ok);
            if (!ok) return false;
            callWith(abi.bronze_super_call,
                     {base, thisVal, builder_.getInt32(argc), argv});
            return true;
        }
        case il::Op::SuperCallSpread: {
            if (!needs(3, true, "Invalid operands for SuperCallSpread")) return false;
            const char* what = "Undefined operand in SuperCallSpread instruction";
            llvm::Value* base = operand(inst, 0, what);
            llvm::Value* thisVal = operand(inst, 1, what);
            llvm::Value* args = operand(inst, 2, what);
            if (!base || !thisVal || !args) return false;
            callWith(abi.bronze_super_call_spread, {base, thisVal, args});
            return true;
        }
        case il::Op::ConstructSpread: {
            if (!needs(2, true, "Invalid operands for ConstructSpread")) return false;
            const char* what = "Undefined operand in ConstructSpread instruction";
            llvm::Value* callee = operand(inst, 0, what);
            llvm::Value* args = operand(inst, 1, what);
            if (!callee || !args) return false;
            callWith(abi.bronze_construct_spread, {callee, args});
            return true;
        }

        case il::Op::CreateFunction: {
            if (inst.result == il::kNoValue) return true;
            llvm::Value* env = inst.operands.empty()
                                   ? builder_.getInt64(BRONZE_ABI_UNDEFINED_BITS)
                                   : operand(inst, 0, "Undefined environment in CreateFunction");
            if (!env) return false;
            if (!require(inst.calleeIndex < shared_.module.functions.size(),
                         "CreateFunction of an out-of-range function index")) {
                return false;
            }
            // The two OWN data properties every function object has (10.2.9,
            // 10.2.10) come off the IL function rather than off the
            // instruction: they are facts about the callee, exactly as the
            // adaptation arity in `FunctionRef` below already is, and one
            // instruction carrying a copy of a callee's fact is one more place
            // for the two to disagree.
            const auto& created = shared_.module.functions[inst.calleeIndex];
            callWith(abi.bronze_create_function,
                     {shared_.wrappers[inst.calleeIndex], builder_.getInt32(inst.immI32),
                      builder_.getInt32(created.requiredArgs),
                      builder_.getInt32(created.nameKeyIndex), env});
            if (created.isGenerator && inst.result != il::kNoValue) {
                builder_.CreateCall(abi.bronze_set_function_generator, {values_[inst.result]});
            }
            return true;
        }
        case il::Op::FunctionRef: {
            if (inst.result == il::kNoValue) return true;
            if (!require(inst.calleeIndex < shared_.wrappers.size(),
                         "FunctionRef to an out-of-range function index")) {
                return false;
            }
            const auto& target = shared_.module.functions[inst.calleeIndex];
            // The arity a call is ADAPTED to: the parameters a caller supplies.
            // A rest parameter is not one of them — padding argv up to it would
            // put an `undefined` in the rest array.
            uint32_t arity = target.adaptArity();
            // The slot is the IL function index: dense, stable, and shared by
            // every mention of one declaration, which is exactly what a
            // singleton's cache line wants to be keyed by.
            values_[inst.result] = emitFunctionSingletonCached(
                builder_, abi, shared_.globals, shared_.wrappers[inst.calleeIndex], arity,
                target.requiredArgs, target.nameKeyIndex, inst.calleeIndex);
            if (target.isGenerator && inst.result != il::kNoValue) {
                builder_.CreateCall(abi.bronze_set_function_generator, {values_[inst.result]});
            }
            return true;
        }
        case il::Op::EnvCreate: {
            if (inst.result == il::kNoValue) return true;
            llvm::Value* parent = inst.operands.empty()
                                      ? builder_.getInt64(BRONZE_ABI_UNDEFINED_BITS)
                                      : operand(inst, 0, "Undefined parent in EnvCreate");
            if (!parent) return false;
            callWith(abi.bronze_env_create, {parent, builder_.getInt32(inst.immI32)});
            return true;
        }
        case il::Op::EnvGet: {
            if (!needs(1, true, "Invalid operands for EnvGet")) return false;
            llvm::Value* env = operand(inst, 0, "Undefined environment in EnvGet");
            if (!env) return false;
            values_[inst.result] = emitEnvGet(builder_, abi, env, inst.envDepth, inst.envIndex,
                                              /*tdz=*/false, inst.keyIndex);
            return true;
        }
        case il::Op::EnvGetTdz: {
            if (!needs(1, true, "Invalid operands for EnvGetTdz")) return false;
            llvm::Value* env = operand(inst, 0, "Undefined environment in EnvGetTdz");
            if (!env) return false;
            values_[inst.result] = emitEnvGet(builder_, abi, env, inst.envDepth, inst.envIndex,
                                              /*tdz=*/true, inst.keyIndex);
            return true;
        }
        // The marker goes in as a plain constant: it has no helper of its own,
        // because no value generated code holds may ever be it. Only a slot can.
        case il::Op::EnvInitTdz: {
            if (!needs(1, false, "Invalid operands for EnvInitTdz")) return false;
            llvm::Value* env = operand(inst, 0, "Undefined environment in EnvInitTdz");
            if (!env) return false;
            builder_.CreateCall(abi.bronze_env_set,
                                {env, builder_.getInt32(inst.envDepth),
                                 builder_.getInt32(inst.envIndex),
                                 builder_.getInt64(BRONZE_ABI_UNINITIALIZED_BITS)});
            return true;
        }
        case il::Op::EnvSet: {
            if (!needs(2, false, "Invalid operands for EnvSet")) return false;
            llvm::Value* env = operand(inst, 0, "Undefined operand in EnvSet");
            llvm::Value* val = operand(inst, 1, "Undefined operand in EnvSet");
            if (!env || !val) return false;
            emitEnvSet(builder_, abi, env, inst.envDepth, inst.envIndex, val);
            return true;
        }

        case il::Op::ModuleEnvSet: {
            if (!needs(1, false, "Invalid operands for ModuleEnvSet")) return false;
            llvm::Value* env = operand(inst, 0, "Undefined environment in ModuleEnvSet");
            if (!env) return false;
            builder_.CreateCall(abi.bronze_module_env_set, {env});
            return true;
        }
        case il::Op::ModuleEnvGet:
            if (inst.result != il::kNoValue) callWith(abi.bronze_module_env_get, {});
            return true;

        case il::Op::Print:
        case il::Op::PrintErr: {
            // One argument keeps the direct call; several go through the
            // argv region of this function's root frame, exactly as a
            // dynamic call's arguments do, so every one of them stays rooted
            // across the helper.
            const bool toStderr = inst.op == il::Op::PrintErr;
            if (inst.operands.size() == 1) {
                if (!values_[inst.operands[0]]) return true;
                builder_.CreateCall(toStderr ? abi.bronze_print_value_err : abi.bronze_print_value,
                                    {values_[inst.operands[0]]});
                return true;
            }
            const uint32_t argc = static_cast<uint32_t>(inst.operands.size());
            bool ok = false;
            llvm::Value* argv = emitArgv(inst, 0, argc, ok);
            if (!ok) return false;
            builder_.CreateCall(toStderr ? abi.bronze_print_values_err : abi.bronze_print_values,
                                {builder_.getInt32(argc), argv});
            return true;
        }

        case il::Op::PrintSpread:
        case il::Op::PrintSpreadErr: {
            const bool toStderr = inst.op == il::Op::PrintSpreadErr;
            if (!inst.operands.empty() && values_[inst.operands[0]]) {
                builder_.CreateCall(toStderr ? abi.bronze_print_spread_err : abi.bronze_print_spread,
                                    {values_[inst.operands[0]]});
            }
            return true;
        }

        case il::Op::PropGet: {
            if (!needs(1, true, "Invalid operands for PropGet")) return false;
            llvm::Value* obj = operand(inst, 0, "Undefined object in PropGet instruction");
            if (!obj) return false;
            // A site inference proved monomorphic gets the guard inlined here;
            // an unproven one keeps the plain call, so the inline form never
            // grows into a polymorphic guard chain in the object file. This may
            // SPLIT the current block.
            const std::string& keyStr = inst.keyIndex < shared_.module.keyConstants.size()
                                            ? shared_.module.keyConstants[inst.keyIndex]
                                            : "";
            values_[inst.result] =
                emitPropGet(builder_, abi, shared_.globals, shared_.icTable, obj, inst.keyIndex,
                            inst.icIndex, inst.icMonomorphic, keyStr);
            if (inst.result < propGetKey_.size()) propGetKey_[inst.result] = inst.keyIndex;
            return true;
        }
        case il::Op::PropSet: {
            if (!needs(2, false, "Invalid operands for PropSet")) return false;
            llvm::Value* obj = operand(inst, 0, "Undefined operand in PropSet instruction");
            llvm::Value* val = operand(inst, 1, "Undefined operand in PropSet instruction");
            if (!obj || !val) return false;
            const std::string& keyStr = inst.keyIndex < shared_.module.keyConstants.size()
                                            ? shared_.module.keyConstants[inst.keyIndex]
                                            : "";
            emitPropSet(builder_, abi, shared_.globals, shared_.icTable, obj, inst.keyIndex, val,
                        inst.icIndex, inst.immI32 != 0, inst.icMonomorphic, keyStr);
            return true;
        }
        case il::Op::ElemGet: {
            if (!needs(2, true, "Invalid operands for ElemGet")) return false;
            llvm::Value* obj = operand(inst, 0, "Undefined operand in ElemGet instruction");
            llvm::Value* idx = operand(inst, 1, "Undefined operand in ElemGet instruction");
            if (!obj || !idx) return false;
            values_[inst.result] = emitElemGet(builder_, abi, obj, idx);
            return true;
        }
        case il::Op::ElemSet: {
            if (!needs(3, false, "Invalid operands for ElemSet")) return false;
            llvm::Value* obj = operand(inst, 0, "Undefined operand in ElemSet instruction");
            llvm::Value* idx = operand(inst, 1, "Undefined operand in ElemSet instruction");
            llvm::Value* val = operand(inst, 2, "Undefined operand in ElemSet instruction");
            if (!obj || !idx || !val) return false;
            emitElemSet(builder_, abi, obj, idx, val, inst.immI32 != 0);
            return true;
        }

        case il::Op::DynamicCall: {
            if (!needs(2, false, "Invalid operands for DynamicCall")) return false;
            llvm::Value* callee =
                operand(inst, 0, "Undefined callee or this in DynamicCall instruction");
            llvm::Value* thisVal =
                operand(inst, 1, "Undefined callee or this in DynamicCall instruction");
            if (!callee || !thisVal) return false;
            uint32_t argc = static_cast<uint32_t>(inst.operands.size() - 2);
            bool ok = false;
            llvm::Value* argv = emitArgv(inst, 2, argc, ok);
            if (!ok) return false;
            // A callee read as `sqrt`/`sin`/`cos`/`abs`/`min`/`max` gets the
            // code-pointer-guarded Math dispatch; the provenance only decides
            // WHERE to spend the guard, never what it may assume.
            const uint32_t calleeKey = inst.operands[0] < propGetKey_.size()
                                           ? propGetKey_[inst.operands[0]]
                                           : UINT32_MAX;
            if (calleeKey < shared_.module.keyConstants.size()) {
                if (inst.result != il::kNoValue) {
                    if (auto kind =
                            mathIntrinsicFor(shared_.module.keyConstants[calleeKey], argc)) {
                        llvm::SmallVector<llvm::Value*, 2> args;
                        for (uint32_t a = 0; a < argc; ++a) {
                            args.push_back(values_[inst.operands[2 + a]]);
                        }
                        values_[inst.result] = emitMathDirectCall(builder_, abi, *kind, callee,
                                                                  thisVal, argc, argv, args);
                        return true;
                    }
                }
                if (shared_.module.keyConstants[calleeKey] == "push" && argc == 1) {
                    llvm::Value* argVal = values_[inst.operands[2]];
                    llvm::Value* res = emitArrayPushDirectCall(
                        builder_, abi, callee, thisVal, argc, argv, argVal);
                    if (inst.result != il::kNoValue) {
                        values_[inst.result] = res;
                    }
                    return true;
                }
            }
            // The inline path skips the helper's NewTargetScope(undefined)
            // push — the mask that makes new.target read undefined inside a
            // plain call made during construction. Same rule as the inline
            // `new` path: bronze_get_new_target is the scope stack's only
            // observer, so one new.target anywhere keeps the whole module on
            // the helper, and its absence makes the skip unobservable.
            if (shared_.moduleHasNewTarget) {
                callWith(abi.bronze_dynamic_call,
                         {callee, thisVal, builder_.getInt32(argc), argv});
                return true;
            }
            llvm::Value* res = emitDynamicCallInline(
                builder_, abi, shared_.globals, callee, thisVal, argc, argv);
            if (inst.result != il::kNoValue) {
                values_[inst.result] = res;
            }
            return true;
        }
        case il::Op::Construct: {
            if (!needs(1, false, "Invalid operands for Construct")) return false;
            llvm::Value* ctor = operand(inst, 0, "Undefined constructor in Construct instruction");
            if (!ctor) return false;
            uint32_t argc = static_cast<uint32_t>(inst.operands.size() - 1);
            bool ok = false;
            llvm::Value* argv = emitArgv(inst, 1, argc, ok);
            if (!ok) return false;
            // The vetted-constructor fast path: bump-allocate the instance
            // and call the constructor's code directly, with every miss one
            // branch into the helper. Off for the whole module when anything
            // in it reads `new.target` (the slot is then never planned) —
            // the inline path does not push the helper's NewTargetScope.
            // This may SPLIT the current block.
            if (constructSelfSlot_ != kNoSlot) {
                llvm::Value* res =
                    emitConstructInline(builder_, abi, shared_.globals, ctor, argc, argv,
                                        slotAddr(constructSelfSlot_));
                if (inst.result != il::kNoValue) values_[inst.result] = res;
                return true;
            }
            callWith(abi.bronze_construct, {ctor, builder_.getInt32(argc), argv});
            return true;
        }
        case il::Op::Call: {
            if (!require(inst.calleeIndex < shared_.entries.size(),
                         "Invalid callee index in Call instruction")) {
                return false;
            }
            llvm::Function* callee = shared_.entries[inst.calleeIndex];
            std::vector<llvm::Value*> args;
            args.reserve(inst.operands.size());
            for (size_t i = 0; i < inst.operands.size(); ++i) {
                llvm::Value* arg = operand(inst, i, "Undefined argument in Call instruction");
                if (!arg) return false;
                args.push_back(arg);
            }
            llvm::Value* res = builder_.CreateCall(callee, args);
            if (inst.result != il::kNoValue && !callee->getReturnType()->isVoidTy()) {
                values_[inst.result] = res;
            }
            return true;
        }

        case il::Op::IsNullish: {
            if (!needs(1, true, "Invalid operands for IsNullish")) return false;
            callWith(abi.bronze_is_nullish, {values_[inst.operands[0]]});
            return true;
        }

        case il::Op::TypeOf: {
            if (!needs(1, true, "Invalid operands for TypeOf")) return false;
            llvm::Value* v = operand(inst, 0, "Undefined operand in TypeOf instruction");
            if (!v) return false;
            callWith(abi.bronze_typeof, {v});
            return true;
        }
        // 7.1.17, which can run a user `toString` and so can throw. It is on
        // no cannot-throw list for that reason; the caller tests the pending
        // cell right after, like every other helper that may raise.
        case il::Op::ToStr: {
            if (!needs(1, true, "Invalid operands for ToStr")) return false;
            llvm::Value* v = operand(inst, 0, "Undefined operand in ToStr instruction");
            if (!v) return false;
            callWith(abi.bronze_to_string, {v});
            return true;
        }
        case il::Op::InstanceOf:
        case il::Op::In: {
            const bool isIn = inst.op == il::Op::In;
            if (!needs(2, true, isIn ? "Invalid operands for In"
                                     : "Invalid operands for InstanceOf")) {
                return false;
            }
            const char* what = "Undefined operand in a relational-predicate instruction";
            llvm::Value* left = operand(inst, 0, what);
            llvm::Value* right = operand(inst, 1, what);
            if (!left || !right) return false;
            callWith(isIn ? abi.bronze_has_property : abi.bronze_instanceof, {left, right});
            return true;
        }

        default:
            return require(false, "Unsupported IL instruction opcode");
    }
}

}  // namespace bronze::codegen_llvm
