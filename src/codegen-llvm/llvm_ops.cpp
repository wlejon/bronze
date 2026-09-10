// The IL operations that become a runtime helper call or a constant.
// Arithmetic is llvm_arith.cpp; control flow is the terminator half of
// llvm_func.cpp; the calls are llvm_ops_call.cpp; every op that reads or
// writes a property of an object is llvm_ops_access.cpp.

#include <string>

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/MDBuilder.h>

#include "abi/bronze_abi.h"
#include "codegen-llvm/llvm_cache.h"
#include "codegen-llvm/llvm_call.h"
#include "codegen-llvm/llvm_construct.h"
#include "codegen-llvm/llvm_elem.h"
#include "codegen-llvm/llvm_iter.h"
#include "codegen-llvm/llvm_env.h"
#include "codegen-llvm/llvm_func.h"
#include "codegen-llvm/llvm_math.h"
#include "codegen-llvm/llvm_pin.h"
#include "codegen-llvm/llvm_prop.h"
#include "codegen-llvm/llvm_repr.h"

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
                values_[inst.result] = builder_.CreateCall(
                    abi.bronze_bigint_literal, {emitKeyId(builder_, shared_.tables, inst.keyIndex)});
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
                builder_.CreateLoad(i64Ty_, globals_.bronze_exception_cell);
            builder_.CreateStore(builder_.getInt64(BRONZE_ABI_NO_EXCEPTION_BITS),
                                 globals_.bronze_exception_cell);
            return true;
        }

        case il::Op::Box:
            return emitBox(inst);
        case il::Op::Unbox:
            return emitUnbox(inst);

        case il::Op::CreateObject:
            if (inst.result != il::kNoValue) {
                values_[inst.result] = emitCreateObjectInline(builder_, abi, globals_);
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
            // The second argument is the importing module's URL key: the
            // base a host resolves a relative specifier against.
            callWith(abi.bronze_dynamic_import,
                     {spec, emitKeyId(builder_, shared_.tables, inst.keyIndex)});
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
                // A small literal — the `[x, z]` tuple a geometry helper
                // returns, the `[a, b, c]` a table row is — is bump-allocated
                // inline (llvm_construct.h); its HOLE fill is `capacity`
                // stores, which is why the size is bounded here.
                if (inst.immI32 >= 0 && inst.immI32 <= 8) {
                    values_[inst.result] = emitCreateArrayInline(
                        builder_, abi, globals_, static_cast<uint32_t>(inst.immI32));
                } else {
                    callWith(abi.bronze_create_array, {builder_.getInt32(inst.immI32)});
                }
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
                     {target, emitKeyId(builder_, shared_.tables, inst.keyIndex), value});
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
                     {target, emitKeyId(builder_, shared_.tables, inst.keyIndex), getter, setter,
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
                callWith(abi.bronze_import_meta,
                         {emitKeyId(builder_, shared_.tables, inst.keyIndex)});
            }
            return true;
        // The access family — every op that reads or writes a property of an
        // object — is llvm_ops_access.cpp, where the receiver proofs live.
        case il::Op::SuperGet:
        case il::Op::SuperSet:
        case il::Op::PropDelete:
        case il::Op::ElemDelete:
        case il::Op::PropGet:
        case il::Op::PropSet:
        case il::Op::ElemGet:
        case il::Op::ElemSet:
        case il::Op::ElemGetTyped:
        case il::Op::ElemSetTyped:
        // A property WRITE — one with attributes an assignment cannot name —
        // so it belongs with the accesses even though it arms no proof.
        case il::Op::DefineOwnAttr:
        // Not an access, but the CLAIM a run of accesses is licensed by, and it
        // must be emitted out of the same ladder they are proven with.
        case il::Op::IsDenseArray:
            return emitAccessOp(inst);
        case il::Op::GlobalGet:
            if (inst.result != il::kNoValue) {
                auto it = cachedGlobalGets_.find(inst.keyIndex);
                if (it != cachedGlobalGets_.end() && it->second != nullptr) {
                    values_[inst.result] = it->second;
                } else {
                    llvm::Value* res =
                        emitGlobalGetCached(builder_, abi, shared_.tables, inst.keyIndex);
                    values_[inst.result] = res;
                    cachedGlobalGets_[inst.keyIndex] = res;
                }
            }
            return true;
        // The result is READ: the helper answers with the global object's own
        // property when the program made one, and only raises when it did not.
        // `il::canThrow` therefore still guards it — the check may fire, where
        // for `immutable.assign` below it always does.
        case il::Op::ResolveName:
            callWith(abi.bronze_resolve_name,
                     {emitKeyId(builder_, shared_.tables, inst.keyIndex),
                      builder_.getInt1(inst.immI32 != 0)});
            return true;
        // The helper always raises, the exception check after it always fires,
        // and the `undefined` it returns is materialized only so the verifier
        // sees a definition for the value id.
        case il::Op::ImmutableAssign:
            callWith(abi.bronze_immutable_assign, {});
            return true;
        // The `--pins` write barrier (llvm_pin.h). Void, so nothing is
        // materialized for it; what carries the violation out is the
        // exception check `il::canThrow` puts immediately after, which is
        // also what makes the STORE this precedes not happen.
        case il::Op::PinGuard: {
            if (!needs(1, false, "Invalid operands for PinGuard")) return false;
            llvm::Value* val = operand(inst, 0, "Undefined operand in PinGuard instruction");
            if (!val) return false;
            // The barrier owns its own raise: the violating arm goes straight
            // to this block's handler, which is where the exception check after
            // a throwing instruction would have sent it (llvm_pin.h). That is
            // why `il::canThrow` and `il::canCollect` both answer no for the
            // branching form — there is no returning path through the raise.
            emitPinGuard(builder_, abi, shared_.tables, val, inst.keyIndex,
                         static_cast<il::PinBarrier>(inst.immI32),
                         unwindTargetFor(currentILBlock_));
            return true;
        }
        // The PIN CENSUS observation (src/runtime/pin_census.h). One plain call
        // per site, with no inline form and no fast path: a census build is an
        // instrument and is never a build anything is measured on, so the
        // cheapest thing to get right is the one that has the fewest ways to be
        // wrong.
        case il::Op::CensusRecord: {
            if (!needs(1, false, "Invalid operands for CensusRecord")) return false;
            llvm::Value* val = operand(inst, 0, "Undefined operand in CensusRecord instruction");
            if (!val) return false;
            builder_.CreateCall(abi.bronze_census_record,
                                {emitKeyId(builder_, shared_.tables, inst.keyIndex),
                                 builder_.getInt32(static_cast<uint32_t>(inst.immI32)), val});
            return true;
        }
        case il::Op::PrivateNew:
        case il::Op::PrivateHas:
        case il::Op::PrivateGet:
        case il::Op::PrivateAdd:
        case il::Op::PrivateSet:
        case il::Op::PrivateMisuse:
            return emitPrivateOp(inst);
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
                case il::Op::IterOpen:
                    values_[inst.result] = emitIterOpen(builder_, abi, globals_, rec);
                    break;
                case il::Op::AsyncIterOpen: callWith(abi.bronze_async_iter_open, {rec}); break;
                case il::Op::AsyncIterNext: callWith(abi.bronze_async_iter_next, {rec}); break;
                // The two the loop pays per ELEMENT, and the only two with an
                // inline path (codegen-llvm/llvm_iter.h). Both keep the helper
                // as their fallback edge, so the record kinds this cannot walk
                // cost exactly what they cost before.
                case il::Op::IterStep:
                    values_[inst.result] = emitIterStep(builder_, abi, rec);
                    break;
                case il::Op::IterValue:
                    values_[inst.result] = emitIterValue(builder_, abi, rec);
                    break;
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
            if (inst.op == il::Op::AsyncIterClose) {
                builder_.CreateCall(abi.bronze_async_iter_close,
                                    {rec, builder_.getInt1(inst.immI32 != 0)});
            } else {
                emitIterClose(builder_, abi, rec, inst.immI32 != 0);
            }
            return true;
        }
        case il::Op::PatternCheck: {
            if (!needs(1, true, "Invalid operands for PatternCheck")) return false;
            llvm::Value* src = operand(inst, 0, "Undefined source in PatternCheck instruction");
            if (!src) return false;
            // The helper does one thing: raise for `null` or `undefined`. It
            // answers its operand unchanged otherwise (rt_spread.cpp), so the
            // tag test is made here and the call is the raising path alone.
            // The result is the source in both arms — on the raising one the
            // pending-cell test that follows is what leaves the function.
            llvm::LLVMContext& ctx = builder_.getContext();
            llvm::Function* fn = builder_.GetInsertBlock()->getParent();
            llvm::BasicBlock* raiseBb = llvm::BasicBlock::Create(ctx, "pc.raise", fn);
            llvm::BasicBlock* okBb = llvm::BasicBlock::Create(ctx, "pc.ok", fn);
            llvm::Value* tag = builder_.CreateLShr(src, BRONZE_ABI_VALUE_TAG_SHIFT, "pc.tag");
            llvm::Value* isUndef =
                builder_.CreateICmpEQ(tag, builder_.getInt64(BRONZE_ABI_TAG_UNDEFINED));
            llvm::Value* isNull =
                builder_.CreateICmpEQ(tag, builder_.getInt64(BRONZE_ABI_TAG_NULL));
            llvm::MDNode* unlikelyBranch = llvm::MDBuilder(ctx).createBranchWeights(1, 1048576);
            builder_.CreateCondBr(builder_.CreateOr(isUndef, isNull, "pc.nullish"), raiseBb,
                                  okBb, unlikelyBranch);
            builder_.SetInsertPoint(raiseBb);
            builder_.CreateCall(abi.bronze_pattern_check,
                                {src, builder_.getInt32(static_cast<uint32_t>(inst.immI32))});
            builder_.CreateBr(okBb);
            builder_.SetInsertPoint(okBb);
            values_[inst.result] = src;
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
        // One aligned load from the module's own table, at an address the
        // compiler knows: the collector forwards the cell IN PLACE through the
        // span registered at module init, so a load always sees current bits.
        case il::Op::TemplateCached: {
            if (inst.result == il::kNoValue) return true;
            llvm::Value* cell =
                templateSlotPtr(builder_, shared_.tables, static_cast<uint32_t>(inst.immI32));
            if (!require(cell != nullptr, "template.cached names no template slot")) return false;
            values_[inst.result] = builder_.CreateAlignedLoad(builder_.getInt64Ty(), cell,
                                                              llvm::Align(8), "tpl.cached");
            return true;
        }
        case il::Op::TemplateObject: {
            if (!needs(2, true, "Invalid operands for TemplateObject")) return false;
            const char* what = "Undefined operand in TemplateObject instruction";
            llvm::Value* cooked = operand(inst, 0, what);
            llvm::Value* raw = operand(inst, 1, what);
            if (!cooked || !raw) return false;
            llvm::Value* cell =
                templateSlotPtr(builder_, shared_.tables, static_cast<uint32_t>(inst.immI32));
            if (!require(cell != nullptr, "template.object names no template slot")) return false;
            callWith(abi.bronze_template_object, {cooked, raw, cell});
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
            uint32_t flags =
                created.fnFlags | (created.needsEnv ? BRONZE_ABI_FN_FLAG_NEEDS_ENV : 0);
            callWith(abi.bronze_create_function,
                     {shared_.wrappers[inst.calleeIndex], builder_.getInt32(inst.immI32),
                      builder_.getInt32(created.requiredArgs),
                      emitKeyId(builder_, shared_.tables, created.nameKeyIndex),
                      builder_.getInt32(static_cast<int32_t>(flags)), env});
            return true;
        }
        case il::Op::FunctionRef: {
            if (inst.result == il::kNoValue) return true;
            if (!require(inst.calleeIndex < shared_.wrappers.size(),
                         "FunctionRef to an out-of-range function index")) {
                return false;
            }
            if (inst.result < funcRefIndex_.size()) {
                funcRefIndex_[inst.result] = inst.calleeIndex;
            }
            const auto& target = shared_.module.functions[inst.calleeIndex];
            // The arity a call is ADAPTED to: the parameters a caller supplies.
            // A rest parameter is not one of them — padding argv up to it would
            // put an `undefined` in the rest array.
            uint32_t arity = target.adaptArity();
            // The slot is the IL function index: dense, stable, and shared by
            // every mention of one declaration, which is exactly what a
            // singleton's cache line wants to be keyed by.
            uint32_t flags = target.fnFlags | (target.needsEnv ? BRONZE_ABI_FN_FLAG_NEEDS_ENV : 0);
            values_[inst.result] = emitFunctionSingletonCached(
                builder_, abi, shared_.tables, shared_.wrappers[inst.calleeIndex], arity,
                target.requiredArgs, target.nameKeyIndex, flags, inst.calleeIndex);
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
            values_[inst.result] = emitEnvGet(builder_, abi, shared_.tables, env, inst.envDepth,
                                              inst.envIndex, /*tdz=*/false, inst.keyIndex,
                                              envGuardsElided_, inst.envImmutable);
            return true;
        }
        case il::Op::EnvGetTdz: {
            if (!needs(1, true, "Invalid operands for EnvGetTdz")) return false;
            llvm::Value* env = operand(inst, 0, "Undefined environment in EnvGetTdz");
            if (!env) return false;
            values_[inst.result] = emitEnvGet(builder_, abi, shared_.tables, env, inst.envDepth,
                                              inst.envIndex, /*tdz=*/true, inst.keyIndex,
                                              envGuardsElided_);
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
            emitEnvSet(builder_, abi, env, inst.envDepth, inst.envIndex, val, envGuardsElided_,
                       reprNeverPointer(repr_.at(inst.operands[1])));
            return true;
        }

        // A store and a load of the module's own cell, not a helper call: the
        // record belongs to THIS module (llvm_abi.h ModuleTables::moduleEnv),
        // so there is nothing for a helper to arbitrate and nothing a second
        // module in the process can overwrite.
        case il::Op::ModuleEnvSet: {
            if (!needs(1, false, "Invalid operands for ModuleEnvSet")) return false;
            llvm::Value* env = operand(inst, 0, "Undefined environment in ModuleEnvSet");
            if (!env) return false;
            builder_.CreateAlignedStore(env, shared_.tables.moduleEnv, llvm::Align(8));
            return true;
        }
        case il::Op::ModuleEnvGet:
            if (inst.result != il::kNoValue) {
                values_[inst.result] = builder_.CreateAlignedLoad(
                    i64Ty_, shared_.tables.moduleEnv, llvm::Align(8), "module.env");
            }
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

        // A proven pristine-Math call on a machine number: the intrinsic
        // lowers to a bare instruction (sqrtsd / andpd / roundsd), and every
        // admitted function is IEEE-exact, so this and the helper the
        // dynamic path calls produce identical bits (il::MathUnaryFn).
        case il::Op::MathUnary: {
            if (!needs(1, true, "Invalid operands for MathUnary")) return false;
            llvm::Value* x = operand(inst, 0, "Undefined operand in MathUnary instruction");
            if (!x) return false;
            llvm::Intrinsic::ID id;
            switch (static_cast<il::MathUnaryFn>(inst.immI32)) {
                case il::MathUnaryFn::Sqrt: id = llvm::Intrinsic::sqrt; break;
                case il::MathUnaryFn::Abs: id = llvm::Intrinsic::fabs; break;
                case il::MathUnaryFn::Floor: id = llvm::Intrinsic::floor; break;
                case il::MathUnaryFn::Ceil: id = llvm::Intrinsic::ceil; break;
                case il::MathUnaryFn::Trunc: id = llvm::Intrinsic::trunc; break;
                default: return require(false, "Unknown MathUnary function selector");
            }
            values_[inst.result] = builder_.CreateUnaryIntrinsic(id, x);
            return true;
        }

        case il::Op::DynamicCall:
            return emitDynamicCall(inst);
        case il::Op::MethodCall:
            return emitMethodCall(inst);
        case il::Op::MethodCallSpread:
            return emitMethodCallSpread(inst);
        case il::Op::Construct:
            return emitConstruct(inst);
        case il::Op::Call:
            return emitCall(inst);

        case il::Op::IsNullish: {
            if (!needs(1, true, "Invalid operands for IsNullish")) return false;
            callWith(abi.bronze_is_nullish, {values_[inst.operands[0]]});
            return true;
        }

        // The same single compare `pin.guard` and `emitDynamicAdd`'s fast arm
        // make (llvm_pin.cpp, llvm_arith.cpp): a Number's bits are its double's
        // bits and every other tag is above the number range, so "is this a
        // Number" is `bits <=u NUMBER_MAX` and touches no memory. No helper, no
        // branch of its own — the branch is the terminator this feeds.
        case il::Op::IsNumber: {
            if (!needs(1, true, "Invalid operands for IsNumber")) return false;
            llvm::Value* src = operand(inst, 0, "Undefined operand in IsNumber instruction");
            if (!src) return false;
            values_[inst.result] = builder_.CreateICmpULE(
                src, builder_.getInt64(BRONZE_ABI_NUMBER_MAX_BITS), "isnum");
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
