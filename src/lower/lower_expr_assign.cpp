// The assignment operators: `=`, the eleven compound forms, and the three
// logical ones — over each of the four reference kinds the language has
// (`o.k`, `o[k]`, a local binding, an environment slot).
//
// Split out of lower_expr.cpp along the seam 13.15 draws: everything here
// evaluates a REFERENCE, reads through it, combines, and writes back, and the
// four kinds differ only in which pair of instructions the read and the write
// are. Nothing in the other file writes to anything.

#include <string>
#include <vector>

#include "lower/lowerer.h"

namespace bronze::lower {

std::optional<Lowerer::Value> Lowerer::lowerAssignment(const ast::Binary* bin,
                                                       il::Function& ilFn) {
    if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(bin->lhs.get())) {
        // A private target is a different mechanism end to end — no key, no
        // inline cache, a brand check instead of a shape one, and a setter
        // reached through the class-evaluation record rather than the
        // prototype chain (lower_private.cpp).
        if (mem->isPrivate) return lowerPrivateAssignment(bin, ilFn);
        auto objVal = lowerExpr(*mem->object, ilFn);
        if (!objVal) return std::nullopt;
        auto objBoxed = boxValueIfNeeded(*objVal, ilFn);
        uint32_t keyIdx = getKeyConstantIndex(mem->property);

        const bool isLogical = bin->op == ast::BinaryOp::LogicalAndAssign ||
                               bin->op == ast::BinaryOp::LogicalOrAssign ||
                               bin->op == ast::BinaryOp::NullishAssign;

        if (isLogical) {
            il::ValueId curId = ilFn.valueCount++;
            il::Instruction getInst;
            getInst.op = il::Op::PropGet;
            getInst.type = il::Type::Dynamic;
            getInst.result = curId;
            getInst.operands = {objBoxed.id};
            getInst.keyIndex = keyIdx;
            getInst.icIndex = icSiteCounter_++;
            const bool mono = monomorphicPropSite(*mem->object);
            recordPropertyAccess(mem->span.file, mono, mono ? "" : propBailReason(*mem->object));
            getInst.icMonomorphic = mono;
            getInst.icFnRecv = functionBindingReceiver(*mem->object, keyIdx);
            stampStaticSlot(getInst, *mem->object);
            emitInst(ilFn, getInst);
            Value curVal{curId, il::Type::Dynamic};

            il::ValueId condId = il::kNoValue;
            if (bin->op == ast::BinaryOp::NullishAssign) {
                condId = ilFn.valueCount++;
                il::Instruction nullishInst;
                nullishInst.op = il::Op::IsNullish;
                nullishInst.type = il::Type::Bool;
                nullishInst.result = condId;
                nullishInst.operands = {curVal.id};
                emitInst(ilFn, nullishInst);
            } else {
                condId = lowerConditionFromVal(curVal, ilFn).id;
            }

            il::BlockId bRhs = createBlock(ilFn);
            il::BlockId bJoin = createBlock(ilFn);
            size_t entryBlockIdx = currentBlockIdx_;
            auto stateEntry = snapshotVarStates();

            setCurrentBlock(bRhs);
            auto rhsVal = lowerExpr(*bin->rhs, ilFn);
            if (!rhsVal) return std::nullopt;
            emitPinFieldBarrier(*mem->object, mem->property, *rhsVal, ilFn);
            Value storedBoxed = boxValueIfNeeded(*rhsVal, ilFn);
            il::Instruction setInst;
            setInst.op = il::Op::PropSet;
            setInst.type = il::Type::Void;
            setInst.result = il::kNoValue;
            setInst.operands = {objBoxed.id, storedBoxed.id};
            setInst.keyIndex = keyIdx;
            setInst.icIndex = icSiteCounter_++;
            setInst.icMonomorphic = monomorphicPropSite(*mem->object);
            stampStaticSlot(setInst, *mem->object);
            setInst.immI32 = strictFlag();
            emitInst(ilFn, setInst);
            auto stateRhs = snapshotVarStates();
            bool rhsReaches = !currentBlockIsTerminated(ilFn);
            size_t rhsEndBlockIdx = currentBlockIdx_;

            il::Type joinType = il::Type::Dynamic;
            il::ValueId resParamId = ilFn.valueCount++;
            ilFn.blocks[bJoin].params.push_back({resParamId, joinType});
            ExprJoin join = makeExprJoin(stateEntry, stateRhs, bJoin, ilFn);

            setCurrentBlock(entryBlockIdx);
            std::vector<il::ValueId> skipArgs{curVal.id};
            appendExprJoinArgs(skipArgs, join, stateEntry, ilFn);

            il::Instruction brInst;
            brInst.op = il::Op::Branch;
            brInst.type = il::Type::Void;
            brInst.result = il::kNoValue;
            brInst.operands = {condId};
            if (bin->op == ast::BinaryOp::LogicalAndAssign) {
                brInst.target = il::BlockTarget{.block = bRhs, .args = {}};
                brInst.elseTarget = il::BlockTarget{.block = bJoin, .args = std::move(skipArgs)};
            } else if (bin->op == ast::BinaryOp::LogicalOrAssign) {
                brInst.target = il::BlockTarget{.block = bJoin, .args = std::move(skipArgs)};
                brInst.elseTarget = il::BlockTarget{.block = bRhs, .args = {}};
            } else {
                brInst.target = il::BlockTarget{.block = bRhs, .args = {}};
                brInst.elseTarget = il::BlockTarget{.block = bJoin, .args = std::move(skipArgs)};
            }
            ilFn.blocks[entryBlockIdx].instructions.push_back(brInst);

            if (rhsReaches) {
                setCurrentBlock(rhsEndBlockIdx);
                std::vector<il::ValueId> args{storedBoxed.id};
                appendExprJoinArgs(args, join, stateRhs, ilFn);
                il::Instruction jmpInst;
                jmpInst.op = il::Op::Jump;
                jmpInst.type = il::Type::Void;
                jmpInst.result = il::kNoValue;
                jmpInst.target = il::BlockTarget{.block = bJoin, .args = std::move(args)};
                emitInst(ilFn, jmpInst);
            }

            setCurrentBlock(bJoin);
            restoreVarStates(stateEntry);
            bindExprJoinParams(join);
            return Value{resParamId, joinType};
        }

        // Compound assignment reads the current value before the
        // rhs is evaluated (JS evaluation order).
        std::optional<Value> curVal;
        if (bin->op != ast::BinaryOp::Assign) {
            il::ValueId cur = ilFn.valueCount++;
            il::Instruction getInst;
            getInst.op = il::Op::PropGet;
            getInst.type = il::Type::Dynamic;
            getInst.result = cur;
            getInst.operands = {objBoxed.id};
            getInst.keyIndex = keyIdx;
            getInst.icIndex = icSiteCounter_++;
            const bool mono = monomorphicPropSite(*mem->object);
            recordPropertyAccess(mem->span.file, mono, mono ? "" : propBailReason(*mem->object));
            getInst.icMonomorphic = mono;
            getInst.icFnRecv = functionBindingReceiver(*mem->object, keyIdx);
            stampStaticSlot(getInst, *mem->object);
            emitInst(ilFn, getInst);
            curVal = Value{cur, il::Type::Dynamic};
        }

        auto rhsVal = lowerExpr(*bin->rhs, ilFn);
        if (!rhsVal) return std::nullopt;
        Value stored = curVal ? emitCompoundCombine(*curVal, *rhsVal, bin->op,
                                                   provenNumber(*bin), ilFn)
                              : *rhsVal;
        emitPinFieldBarrier(*mem->object, mem->property, stored, ilFn);
        Value storedBoxed = boxValueIfNeeded(stored, ilFn);

        il::Instruction inst;
        inst.op = il::Op::PropSet;
        inst.type = il::Type::Void;
        inst.result = il::kNoValue;
        inst.operands = {objBoxed.id, storedBoxed.id};
        inst.keyIndex = keyIdx;
        inst.icIndex = icSiteCounter_++;
        const bool monoSet = monomorphicPropSite(*mem->object);
        recordPropertyAccess(mem->span.file, monoSet, monoSet ? "" : propBailReason(*mem->object));
        inst.icMonomorphic = monoSet;
        stampStaticSlot(inst, *mem->object);
        // The reference this write goes through is strict exactly when the
        // code that wrote it is (13.15.2 PutValue step 6.d), and that is the
        // only thing that decides whether a refused Set throws.
        inst.immI32 = strictFlag();
        emitInst(ilFn, inst);
        return storedBoxed;
    }
    if (const auto* idxAccess = dynamic_cast<const ast::IndexAccess*>(bin->lhs.get())) {
        // A proven typed-array element store, when the operator's read half
        // (if it has one) is coerced by its own combine: plain `=`, the
        // always-numeric compounds, and `+=` against a definitely numeric
        // RHS. The logical assigns are excluded — they may RETURN the read
        // raw — and take the ordinary path below.
        if (const auto elemKind = typedElemAccessKind(*bin->lhs)) {
            const bool admissible =
                bin->op == ast::BinaryOp::Assign ||
                typedElemCompoundAdmissible(bin->op, *bin->rhs);
            if (admissible) return lowerTypedElemAssign(bin, *idxAccess, *elemKind, ilFn);
        }
        auto objVal = lowerExpr(*idxAccess->object, ilFn);
        if (!objVal) return std::nullopt;
        auto objBoxed = boxValueIfNeeded(*objVal, ilFn);

        const std::optional<uint32_t> literalKey = literalIndexKey(*idxAccess->index);

        std::optional<Value> idxBoxed;
        if (!literalKey) {
            auto indexVal = lowerExpr(*idxAccess->index, ilFn);
            if (!indexVal) return std::nullopt;
            idxBoxed = boxValueIfNeeded(*indexVal, ilFn);
        }

        const bool isLogical = bin->op == ast::BinaryOp::LogicalAndAssign ||
                               bin->op == ast::BinaryOp::LogicalOrAssign ||
                               bin->op == ast::BinaryOp::NullishAssign;

        if (isLogical) {
            il::ValueId curId = ilFn.valueCount++;
            il::Instruction getInst;
            if (literalKey) {
                getInst.op = il::Op::PropGet;
                getInst.operands = {objBoxed.id};
                getInst.keyIndex = *literalKey;
                getInst.icIndex = icSiteCounter_++;
                const bool mono = monomorphicPropSite(*idxAccess->object);
                recordPropertyAccess(idxAccess->span.file, mono, mono ? "" : propBailReason(*idxAccess->object));
                getInst.icMonomorphic = mono;
                getInst.icFnRecv = functionBindingReceiver(*idxAccess->object, *literalKey);
                stampStaticSlot(getInst, *idxAccess->object);
            } else {
                const bool native = provenArrayOrTypedArray(*idxAccess->object);
                recordElementOp(idxAccess->span.file, native, native ? "" : "computed dynamic index");
                getInst.op = il::Op::ElemGet;
                getInst.operands = {objBoxed.id, idxBoxed->id};
            }
            getInst.type = il::Type::Dynamic;
            getInst.result = curId;
            emitInst(ilFn, getInst);
            Value curVal{curId, il::Type::Dynamic};

            il::ValueId condId = il::kNoValue;
            if (bin->op == ast::BinaryOp::NullishAssign) {
                condId = ilFn.valueCount++;
                il::Instruction nullishInst;
                nullishInst.op = il::Op::IsNullish;
                nullishInst.type = il::Type::Bool;
                nullishInst.result = condId;
                nullishInst.operands = {curVal.id};
                emitInst(ilFn, nullishInst);
            } else {
                condId = lowerConditionFromVal(curVal, ilFn).id;
            }

            il::BlockId bRhs = createBlock(ilFn);
            il::BlockId bJoin = createBlock(ilFn);
            size_t entryBlockIdx = currentBlockIdx_;
            auto stateEntry = snapshotVarStates();

            setCurrentBlock(bRhs);
            auto rhsVal = lowerExpr(*bin->rhs, ilFn);
            if (!rhsVal) return std::nullopt;
            if (literalKey) {
                emitPinFieldBarrier(*idxAccess->object, keyStrings_[*literalKey], *rhsVal, ilFn);
            }
            Value storedBoxed = boxValueIfNeeded(*rhsVal, ilFn);

            il::Instruction setInst;
            if (literalKey) {
                const bool mono = monomorphicPropSite(*idxAccess->object);
                recordPropertyAccess(idxAccess->span.file, mono, mono ? "" : propBailReason(*idxAccess->object));
                setInst.op = il::Op::PropSet;
                setInst.operands = {objBoxed.id, storedBoxed.id};
                setInst.keyIndex = *literalKey;
                setInst.icIndex = icSiteCounter_++;
                setInst.icMonomorphic = mono;
                stampStaticSlot(setInst, *idxAccess->object);
            } else {
                const bool native = provenArrayOrTypedArray(*idxAccess->object);
                recordElementOp(idxAccess->span.file, native, native ? "" : "computed dynamic index");
                setInst.op = il::Op::ElemSet;
                setInst.operands = {objBoxed.id, idxBoxed->id, storedBoxed.id};
            }
            setInst.type = il::Type::Void;
            setInst.result = il::kNoValue;
            setInst.immI32 = strictFlag();
            emitInst(ilFn, setInst);

            auto stateRhs = snapshotVarStates();
            bool rhsReaches = !currentBlockIsTerminated(ilFn);
            size_t rhsEndBlockIdx = currentBlockIdx_;

            il::Type joinType = il::Type::Dynamic;
            il::ValueId resParamId = ilFn.valueCount++;
            ilFn.blocks[bJoin].params.push_back({resParamId, joinType});
            ExprJoin join = makeExprJoin(stateEntry, stateRhs, bJoin, ilFn);

            setCurrentBlock(entryBlockIdx);
            std::vector<il::ValueId> skipArgs{curVal.id};
            appendExprJoinArgs(skipArgs, join, stateEntry, ilFn);

            il::Instruction brInst;
            brInst.op = il::Op::Branch;
            brInst.type = il::Type::Void;
            brInst.result = il::kNoValue;
            brInst.operands = {condId};
            if (bin->op == ast::BinaryOp::LogicalAndAssign) {
                brInst.target = il::BlockTarget{.block = bRhs, .args = {}};
                brInst.elseTarget = il::BlockTarget{.block = bJoin, .args = std::move(skipArgs)};
            } else if (bin->op == ast::BinaryOp::LogicalOrAssign) {
                brInst.target = il::BlockTarget{.block = bJoin, .args = std::move(skipArgs)};
                brInst.elseTarget = il::BlockTarget{.block = bRhs, .args = {}};
            } else {
                brInst.target = il::BlockTarget{.block = bRhs, .args = {}};
                brInst.elseTarget = il::BlockTarget{.block = bJoin, .args = std::move(skipArgs)};
            }
            ilFn.blocks[entryBlockIdx].instructions.push_back(brInst);

            if (rhsReaches) {
                setCurrentBlock(rhsEndBlockIdx);
                std::vector<il::ValueId> args{storedBoxed.id};
                appendExprJoinArgs(args, join, stateRhs, ilFn);
                il::Instruction jmpInst;
                jmpInst.op = il::Op::Jump;
                jmpInst.type = il::Type::Void;
                jmpInst.result = il::kNoValue;
                jmpInst.target = il::BlockTarget{.block = bJoin, .args = std::move(args)};
                emitInst(ilFn, jmpInst);
            }

            setCurrentBlock(bJoin);
            restoreVarStates(stateEntry);
            bindExprJoinParams(join);
            return Value{resParamId, joinType};
        }

        // Compound assignment reads the current element before
        // the rhs is evaluated (JS evaluation order).
        std::optional<Value> curVal;
        if (bin->op != ast::BinaryOp::Assign) {
            il::ValueId cur = ilFn.valueCount++;
            il::Instruction getInst;
            if (literalKey) {
                getInst.op = il::Op::PropGet;
                getInst.operands = {objBoxed.id};
                getInst.keyIndex = *literalKey;
                getInst.icIndex = icSiteCounter_++;
                const bool mono = monomorphicPropSite(*idxAccess->object);
                recordPropertyAccess(idxAccess->span.file, mono, mono ? "" : propBailReason(*idxAccess->object));
                getInst.icMonomorphic = mono;
                getInst.icFnRecv = functionBindingReceiver(*idxAccess->object, *literalKey);
                stampStaticSlot(getInst, *idxAccess->object);
            } else {
                const bool native = provenArrayOrTypedArray(*idxAccess->object);
                recordElementOp(idxAccess->span.file, native, native ? "" : "computed dynamic index");
                getInst.op = il::Op::ElemGet;
                getInst.operands = {objBoxed.id, idxBoxed->id};
            }
            getInst.type = il::Type::Dynamic;
            getInst.result = cur;
            emitInst(ilFn, getInst);
            curVal = Value{cur, il::Type::Dynamic};
        }

        auto rhsVal = lowerExpr(*bin->rhs, ilFn);
        if (!rhsVal) return std::nullopt;
        Value stored = curVal ? emitCompoundCombine(*curVal, *rhsVal, bin->op,
                                                   provenNumber(*bin), ilFn)
                              : *rhsVal;
        if (literalKey) {
            emitPinFieldBarrier(*idxAccess->object, keyStrings_[*literalKey], stored, ilFn);
        }
        Value storedBoxed = boxValueIfNeeded(stored, ilFn);

        il::Instruction setInst;
        if (literalKey) {
            const bool mono = monomorphicPropSite(*idxAccess->object);
            recordPropertyAccess(idxAccess->span.file, mono, mono ? "" : propBailReason(*idxAccess->object));
            setInst.op = il::Op::PropSet;
            setInst.operands = {objBoxed.id, storedBoxed.id};
            setInst.keyIndex = *literalKey;
            setInst.icIndex = icSiteCounter_++;
            setInst.icMonomorphic = mono;
            stampStaticSlot(setInst, *idxAccess->object);
        } else {
            const bool native = provenArrayOrTypedArray(*idxAccess->object);
            recordElementOp(idxAccess->span.file, native, native ? "" : "computed dynamic index");
            setInst.op = il::Op::ElemSet;
            setInst.operands = {objBoxed.id, idxBoxed->id, storedBoxed.id};
        }
        setInst.type = il::Type::Void;
        setInst.result = il::kNoValue;
        setInst.immI32 = strictFlag();
        emitInst(ilFn, setInst);
        return storedBoxed;
    }
    if (const auto* sm = dynamic_cast<const ast::SuperMember*>(bin->lhs.get())) {
        ast::Ident baseIdent;
        baseIdent.name = sm->baseName;
        baseIdent.span = sm->span;
        auto baseVal = lowerExpr(baseIdent, ilFn);
        if (!baseVal) return std::nullopt;
        auto protoVal = emitPrototypeOf(boxValueIfNeeded(*baseVal, ilFn), ilFn);
        auto thisVal = lowerThisValue(sm->span, ilFn);
        if (!thisVal) return std::nullopt;

        const bool compound = bin->op != ast::BinaryOp::Assign;
        std::optional<Value> curVal;
        if (compound) {
            il::ValueId readId = ilFn.valueCount++;
            il::Instruction readInst;
            readInst.op = il::Op::SuperGet;
            readInst.type = il::Type::Dynamic;
            readInst.result = readId;
            readInst.operands = {protoVal.id, boxValueIfNeeded(*thisVal, ilFn).id};
            readInst.keyIndex = getKeyConstantIndex(sm->property);
            emitInst(ilFn, readInst);
            curVal = Value{readId, il::Type::Dynamic};
        }

        auto rhsVal = lowerExpr(*bin->rhs, ilFn);
        if (!rhsVal) return std::nullopt;
        auto stored = compound ? emitCompoundCombine(*curVal, *rhsVal, bin->op,
                                                     provenNumber(*bin), ilFn)
                               : *rhsVal;
        auto storedBoxed = boxValueIfNeeded(stored, ilFn);

        il::Instruction setInst;
        setInst.op = il::Op::SuperSet;
        setInst.type = il::Type::Void;
        setInst.result = il::kNoValue;
        setInst.operands = {protoVal.id, boxValueIfNeeded(*thisVal, ilFn).id, storedBoxed.id};
        setInst.keyIndex = getKeyConstantIndex(sm->property);
        // The reference this write goes through is strict exactly when the code
        // it was written in is, which for `super.k = v` is every class body
        // (15.7) and any other body a directive raised.
        setInst.immI32 = strictFlag();
        emitInst(ilFn, setInst);
        return storedBoxed;
    }
    if (const auto* ident = dynamic_cast<const ast::Ident*>(bin->lhs.get())) {
        const bool isLogical = bin->op == ast::BinaryOp::LogicalAndAssign ||
                               bin->op == ast::BinaryOp::LogicalOrAssign ||
                               bin->op == ast::BinaryOp::NullishAssign;
        const bool compound = bin->op != ast::BinaryOp::Assign;

        auto it = activeVarMap_.find(ident->name);
        const bool isLocal = it != activeVarMap_.end();
        const size_t bindingIdx = isLocal ? it->second : 0;
        uint32_t depth = 0;
        uint32_t index = 0;
        if (!isLocal) {
            if (currentEnvValue_ == il::kNoValue ||
                !findEnclosingEnvVar(ident->name, depth, index)) {
                if (!resolvesName(ident->name)) {
                    if (!compound && !lowerExpr(*bin->rhs, ilFn)) return std::nullopt;
                    return emitReferenceError(ident->name, ident->span, ilFn);
                }
                diags_.error(ident->span, "cannot assign to '" + ident->name + "'");
                return std::nullopt;
            }
        }

        if (isLogical) {
            Value curVal = isLocal ? readBinding(varBindings_[bindingIdx], ilFn)
                                   : emitEnvGet(depth, index, ilFn);
            il::ValueId condId = il::kNoValue;
            if (bin->op == ast::BinaryOp::NullishAssign) {
                if (curVal.type != il::Type::Dynamic) {
                    return curVal;
                }
                condId = ilFn.valueCount++;
                il::Instruction nullishInst;
                nullishInst.op = il::Op::IsNullish;
                nullishInst.type = il::Type::Bool;
                nullishInst.result = condId;
                nullishInst.operands = {curVal.id};
                emitInst(ilFn, nullishInst);
            } else {
                condId = lowerConditionFromVal(curVal, ilFn).id;
            }

            il::BlockId bRhs = createBlock(ilFn);
            il::BlockId bJoin = createBlock(ilFn);
            size_t entryBlockIdx = currentBlockIdx_;
            auto stateEntry = snapshotVarStates();

            setCurrentBlock(bRhs);
            auto rhsVal = lowerExpr(*bin->rhs, ilFn);
            if (!rhsVal) return std::nullopt;
            Value rhsStored = *rhsVal;
            if (isLocal) {
                if (!refuseConstAssignment(varBindings_[bindingIdx], ilFn)) {
                    writeBinding(varBindings_[bindingIdx], rhsStored, ilFn);
                }
            } else {
                emitEnvSet(depth, index, rhsStored, ilFn, /*assigning=*/true);
            }
            auto stateRhs = snapshotVarStates();
            bool rhsReaches = !currentBlockIsTerminated(ilFn);
            size_t rhsEndBlockIdx = currentBlockIdx_;

            il::Type joinType = (curVal.type == rhsStored.type) ? curVal.type : il::Type::Dynamic;
            il::ValueId resParamId = ilFn.valueCount++;
            ilFn.blocks[bJoin].params.push_back({resParamId, joinType});
            ExprJoin join = makeExprJoin(stateEntry, stateRhs, bJoin, ilFn);

            setCurrentBlock(entryBlockIdx);
            Value curConv = (joinType == il::Type::Dynamic) ? boxValueIfNeeded(curVal, ilFn)
                                                            : unboxValueIfNeeded(curVal, joinType, ilFn);
            std::vector<il::ValueId> skipArgs{curConv.id};
            appendExprJoinArgs(skipArgs, join, stateEntry, ilFn);

            il::Instruction brInst;
            brInst.op = il::Op::Branch;
            brInst.type = il::Type::Void;
            brInst.result = il::kNoValue;
            brInst.operands = {condId};
            if (bin->op == ast::BinaryOp::LogicalAndAssign) {
                brInst.target = il::BlockTarget{.block = bRhs, .args = {}};
                brInst.elseTarget = il::BlockTarget{.block = bJoin, .args = std::move(skipArgs)};
            } else if (bin->op == ast::BinaryOp::LogicalOrAssign) {
                brInst.target = il::BlockTarget{.block = bJoin, .args = std::move(skipArgs)};
                brInst.elseTarget = il::BlockTarget{.block = bRhs, .args = {}};
            } else {
                brInst.target = il::BlockTarget{.block = bRhs, .args = {}};
                brInst.elseTarget = il::BlockTarget{.block = bJoin, .args = std::move(skipArgs)};
            }
            ilFn.blocks[entryBlockIdx].instructions.push_back(brInst);

            if (rhsReaches) {
                setCurrentBlock(rhsEndBlockIdx);
                Value rhsConv = (joinType == il::Type::Dynamic) ? boxValueIfNeeded(rhsStored, ilFn)
                                                                : unboxValueIfNeeded(rhsStored, joinType, ilFn);
                std::vector<il::ValueId> args{rhsConv.id};
                appendExprJoinArgs(args, join, stateRhs, ilFn);
                il::Instruction jmpInst;
                jmpInst.op = il::Op::Jump;
                jmpInst.type = il::Type::Void;
                jmpInst.result = il::kNoValue;
                jmpInst.target = il::BlockTarget{.block = bJoin, .args = std::move(args)};
                emitInst(ilFn, jmpInst);
            }

            setCurrentBlock(bJoin);
            restoreVarStates(stateEntry);
            bindExprJoinParams(join);
            return Value{resParamId, joinType};
        }

        std::optional<Value> curVal;
        if (compound) {
            curVal = isLocal ? readBinding(varBindings_[bindingIdx], ilFn)
                             : emitEnvGet(depth, index, ilFn);
        }

        const bool rhsCoerces = compound && binaryCoercesOperand(bin->op, *bin->lhs);
        auto rhsVal = compound ? (rhsCoerces ? lowerCoercingOperand(*bin->rhs, ilFn)
                                             : lowerExpr(*bin->rhs, ilFn))
                               : lowerNamedEvaluation(*bin->rhs, ident->name, ilFn);
        if (!rhsVal) return std::nullopt;

        Value stored = compound ? emitCompoundCombine(*curVal, *rhsVal, bin->op,
                                                      provenNumber(*bin), ilFn)
                                : *rhsVal;
        if (isLocal) {
            if (!refuseConstAssignment(varBindings_[bindingIdx], ilFn)) {
                writeBinding(varBindings_[bindingIdx], stored, ilFn);
            }
        } else {
            emitEnvSet(depth, index, stored, ilFn, /*assigning=*/true);
        }
        return stored;
    }
    // An array or object literal on the left never arrives here: the parser
    // refines it into a `DestructuringAssign` the moment it sees the `=`, which
    // is a node of its own and not an assignment with a strange target.
    diags_.error(bin->span, "invalid assignment target");
    return std::nullopt;
}

// Where `this` comes from in the function being lowered. An ordinary function
// receives it as the synthetic leading parameter; an arrow has no receiver of
// its own and reads the enclosing function's out of the environment chain,
// under the name no source binding can spell.

}  // namespace bronze::lower
