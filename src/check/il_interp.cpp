#include "check/il_interp_vm.h"

namespace bronze::check {

std::string ConcreteVM::getStr(const CVal& v) const {
    if (v.kind == CVal::Kind::String) return v.str;
    if (v.objId > 0 && v.objId < heap.size()) {
        const auto& obj = heap[v.objId];
        auto lIt = obj.props.find("length");
        if (lIt != obj.props.end() && lIt->second.i64 >= 0 && lIt->second.i64 <= 32) {
            std::string s;
            for (int64_t i = 0; i < lIt->second.i64; ++i) {
                auto cIt = obj.props.find(std::to_string(i));
                s.push_back(static_cast<char>((cIt != obj.props.end()) ? (cIt->second.i64 & 0x7F) : 0));
            }
            return s;
        }
    }
    return std::to_string(v.i64);
}

CVal ConcreteVM::getProp(const CVal& recv, const std::string& key) {
    if (recv.kind == CVal::Kind::String && key == "length") {
        return CVal::makeI64(static_cast<int64_t>(recv.str.size()));
    }
    if (recv.objId == 0 || recv.objId >= heap.size()) return CVal::makeUndef();
    const auto& obj = heap[recv.objId];
    if (key == "constructor" && obj.ctorIdx != UINT32_MAX) {
        CVal fv = CVal::makeObj(allocObj("__fn_" + std::to_string(obj.ctorIdx)));
        fv.kind = CVal::Kind::Function;
        fv.fnIdx = obj.ctorIdx;
        fv.envId = funcCapturedEnvs.count(obj.ctorIdx) ? funcCapturedEnvs[obj.ctorIdx] : 0;
        return fv;
    }
    auto it = obj.props.find(key);
    if (it != obj.props.end()) return it->second;
    if (key == "length") {
        if (!obj.elems.empty()) return CVal::makeI64(static_cast<int64_t>(obj.elems.size()));
        if (obj.rootPath.rfind("__local_", 0) != 0) return CVal::makeI64(1);
        return CVal::makeI64(0);
    }
    if (obj.rootPath.rfind("__local_", 0) == 0) return CVal::makeUndef();
    CVal def = CVal::makeI64(0);
    if (!obj.rootPath.empty() && obj.rootPath.rfind("__fn_", 0) != 0) {
        uint32_t subId = allocObj(obj.rootPath + "." + key);
        def.objId = subId;
        heap[recv.objId].props[key] = def;
    }
    return def;
}

void ConcreteVM::setProp(const CVal& recv, const std::string& key, const CVal& val) {
    if (recv.objId == 0 || recv.objId >= heap.size()) return;
    auto& obj = heap[recv.objId];
    obj.props[key] = val;
    if (!obj.rootPath.empty() && obj.rootPath.rfind("__local_", 0) != 0 && obj.rootPath.rfind("__fn_", 0) != 0) {
        recordedWrites[obj.rootPath + "." + key] = val.i64;
    }
    if (key == "length") {
        for (int64_t i = std::max<int64_t>(0, val.i64); i < 16; ++i) {
            std::string k = std::to_string(i);
            if (obj.props.count(k)) {
                obj.props[k] = CVal::makeI64(0);
                if (!obj.rootPath.empty() && obj.rootPath.rfind("__local_", 0) != 0 && obj.rootPath.rfind("__fn_", 0) != 0) {
                    recordedWrites[obj.rootPath + "." + k] = 0;
                }
            }
        }
    }
}

InterpResult ConcreteVM::evalFn(const il::Function& fn,
                                const std::vector<CVal>& args,
                                CVal thisVal,
                                uint32_t callerEnvId,
                                uint32_t depth,
                                bool isConstruct,
                                CVal* outRetVal) {
    InterpResult res;
    if (fn.blocks.empty() || fn.isExternal || depth > 12) {
        res.failReason = "external or deep recursion";
        return res;
    }
    uint32_t fnIdx = UINT32_MAX;
    for (size_t fi = 0; fi < mod.functions.size(); ++fi) {
        if (&mod.functions[fi] == &fn) { fnIdx = static_cast<uint32_t>(fi); break; }
    }
    std::unordered_map<il::ValueId, CVal> vals;
    auto getV = [&](il::ValueId id) -> CVal {
        auto it = vals.find(id);
        return (it != vals.end()) ? it->second : CVal::makeI64(0);
    };
    uint32_t firstSrc = static_cast<uint32_t>(fn.firstSourceParam());
    if (fn.needsEnv) {
        CVal envV = CVal::makeI64(0);
        if (callerEnvId != 0) envV.envId = callerEnvId;
        else if (fnIdx != UINT32_MAX && funcCapturedEnvs.count(fnIdx)) envV.envId = funcCapturedEnvs[fnIdx];
        vals[0] = envV;
    }
    if (fn.needsThis) {
        vals[fn.needsEnv ? 1 : 0] = thisVal;
    }
    uint32_t srcCount = static_cast<uint32_t>(fn.callerParamCount());
    for (uint32_t i = 0; i < srcCount; ++i) {
        vals[firstSrc + i] = (i < args.size()) ? args[i] : CVal::makeUndef();
    }

    il::BlockId blockId = 0;
    while (stepsLeft > 0) {
        --stepsLeft;
        if (blockId >= fn.blocks.size()) {
            res.failReason = "invalid block";
            return res;
        }
        const auto& blk = fn.blocks[blockId];
        bool jumped = false;
        for (size_t instIdx = 0; instIdx < blk.instructions.size(); ++instIdx) {
            const auto& inst = blk.instructions[instIdx];
            using Op = il::Op;
            auto opV = [&](size_t k) { return (k < inst.operands.size()) ? getV(inst.operands[k]) : CVal::makeUndef(); };
            switch (inst.op) {
                case Op::ConstI32: vals[inst.result] = CVal::makeI64(inst.immI32); break;
                case Op::ConstF64: vals[inst.result] = CVal::makeF64(inst.immF64); break;
                case Op::ConstBool: vals[inst.result] = CVal::makeBool(inst.immI32 != 0); break;
                case Op::ConstNull: vals[inst.result] = CVal::makeNull(); break;
                case Op::ConstUndefined: vals[inst.result] = CVal::makeUndef(); break;
                case Op::IsNullish: {
                    CVal v = opV(0);
                    vals[inst.result] = CVal::makeBool(v.kind == CVal::Kind::Undefined || v.kind == CVal::Kind::Null);
                    break;
                }
                case Op::ToInt32: vals[inst.result] = CVal::makeI64(jsToInt32(opV(0).num)); break;
                case Op::Box:
                    vals[inst.result] = (inst.operands.empty() && inst.keyIndex < mod.keyConstants.size())
                                            ? CVal::makeStr(mod.keyConstants[inst.keyIndex]) : opV(0);
                    break;
                case Op::Unbox: case Op::ToNumeric: case Op::ToStr: vals[inst.result] = opV(0); break;
                case Op::NumTruthy: vals[inst.result] = CVal::makeBool(opV(0).isTruthy()); break;
                case Op::TypeOf: {
                    CVal v = opV(0);
                    std::string t = (v.kind == CVal::Kind::Undefined) ? "undefined"
                                  : (v.kind == CVal::Kind::String) ? "string"
                                  : (v.kind == CVal::Kind::Bool) ? "boolean"
                                  : (v.kind == CVal::Kind::Function) ? "function"
                                  : (v.kind == CVal::Kind::Object || v.kind == CVal::Kind::Null) ? "object" : "number";
                    vals[inst.result] = CVal::makeStr(t);
                    break;
                }
                case Op::GetNewTarget: vals[inst.result] = isConstruct ? CVal::makeBool(true) : CVal::makeUndef(); break;
                case Op::InstanceOf: {
                    CVal l = opV(0), r = opV(1);
                    if (r.kind == CVal::Kind::Function) {
                        bool match = (l.objId > 0 && l.objId < heap.size() && heap[l.objId].ctorIdx == r.fnIdx);
                        vals[inst.result] = CVal::makeBool(match);
                        break;
                    }
                    if (inModuleInit) { vals[inst.result] = CVal::makeBool(false); break; }
                    res.failReason = "unsupported instanceof";
                    return res;
                }
                case Op::ClosureIs: case Op::IsNumber: case Op::IsDenseArray: case Op::IsNumberArray:
                case Op::IsShape: case Op::IsArray: case Op::IsTypedArray: case Op::LayoutEpoch:
                case Op::LayoutFact: case Op::MethodIcIs: case Op::AccessorIcIs:
                    if (inst.result != il::kNoValue) vals[inst.result] = CVal::makeBool(true);
                    break;
                case Op::ClosureEnv: { CVal v = CVal::makeI64(0); v.envId = opV(0).envId; vals[inst.result] = v; break; }
                case Op::ClassExtend:
                case Op::PinGuard: case Op::CensusRecord: case Op::FeedbackRecord: case Op::KeepAlive: case Op::EnvInitTdz: break;
                case Op::CreateObject: case Op::CreateArray: {
                    uint32_t oid = allocObj("__local_" + std::to_string(heap.size()));
                    heap[oid].props["length"] = CVal::makeI64(std::max(0, inst.immI32));
                    vals[inst.result] = CVal::makeObj(oid, static_cast<int64_t>(0x00B1000000000000ULL + oid));
                    break;
                }
                case Op::ArrayAppend: {
                    CVal arr = opV(0), elem = opV(1);
                    if (arr.objId > 0 && arr.objId < heap.size()) {
                        size_t idx = heap[arr.objId].elems.size();
                        heap[arr.objId].elems.push_back(elem);
                        setProp(arr, std::to_string(idx), elem);
                        setProp(arr, "length", CVal::makeI64(static_cast<int64_t>(idx + 1)));
                    }
                    break;
                }
                case Op::ArraySpread: {
                    CVal arr = opV(0), src = opV(1);
                    int64_t dstLen = std::clamp<int64_t>(getProp(arr, "length").i64, 0, 16);
                    int64_t srcLen = std::clamp<int64_t>(getProp(src, "length").i64, 0, 16);
                    for (int64_t i = 0; i < srcLen && dstLen + i < 16; ++i) {
                        setProp(arr, std::to_string(dstLen + i), getProp(src, std::to_string(i)));
                    }
                    setProp(arr, "length", CVal::makeI64(dstLen + srcLen));
                    break;
                }
                case Op::CreateFunction: case Op::FunctionRef: {
                    CVal fv = CVal::makeObj(allocObj("__fn_" + std::to_string(inst.calleeIndex)));
                    fv.kind = CVal::Kind::Function;
                    fv.fnIdx = inst.calleeIndex;
                    fv.envId = (inst.op == Op::CreateFunction && !inst.operands.empty()) ? opV(0).envId : 0;
                    funcCapturedEnvs[inst.calleeIndex] = fv.envId;
                    vals[inst.result] = fv;
                    break;
                }
                case Op::MethodDef: {
                    CVal recv = opV(0), fnV = opV(1);
                    if (recv.objId > 0 && recv.objId < heap.size() && inst.keyIndex < mod.keyConstants.size()) {
                        heap[recv.objId].methods[mod.keyConstants[inst.keyIndex]] = fnV.fnIdx;
                    }
                    break;
                }
                case Op::PropGet: {
                    if (inst.keyIndex >= mod.keyConstants.size()) { res.failReason = "bad key"; return res; }
                    const std::string& key = mod.keyConstants[inst.keyIndex];
                    CVal recv = opV(0);
                    if (key == "prototype" && recv.kind == CVal::Kind::Function) {
                        moduleCtorIdx = recv.fnIdx;
                    }
                    if (!recv.globalName.empty()) {
                        if (recv.globalName == "Number") {
                            if (key == "NaN") { vals[inst.result] = CVal::makeF64(std::numeric_limits<double>::quiet_NaN()); break; }
                            if (key == "POSITIVE_INFINITY") { vals[inst.result] = CVal::makeF64(std::numeric_limits<double>::infinity()); break; }
                            if (key == "NEGATIVE_INFINITY") { vals[inst.result] = CVal::makeF64(-std::numeric_limits<double>::infinity()); break; }
                            if (key == "EPSILON") { vals[inst.result] = CVal::makeF64(std::numeric_limits<double>::epsilon()); break; }
                            if (key == "MAX_SAFE_INTEGER") { vals[inst.result] = CVal::makeF64(9007199254740991.0); break; }
                            if (key == "MIN_SAFE_INTEGER") { vals[inst.result] = CVal::makeF64(-9007199254740991.0); break; }
                        }
                        CVal gv = CVal::makeI64(1);
                        gv.globalName = recv.globalName + "." + key;
                        vals[inst.result] = gv;
                        break;
                    }
                    vals[inst.result] = getProp(recv, key);
                    break;
                }
                case Op::PropSet:
                case Op::DefineOwnAttr: {
                    if (inst.keyIndex >= mod.keyConstants.size()) { res.failReason = "bad key"; return res; }
                    const std::string& key = mod.keyConstants[inst.keyIndex];
                    CVal recv = opV(0), val = opV(1);
                    if (key == "prototype" && recv.kind == CVal::Kind::Function) {
                        moduleCtorIdx = recv.fnIdx;
                    }
                    setProp(recv, key, val);
                    if (inst.result != il::kNoValue) vals[inst.result] = val;
                    break;
                }
                case Op::ElemGet:
                case Op::ElemGetTyped: {
                    CVal recv = opV(0), idx = opV(1);
                    if (recv.kind == CVal::Kind::String) {
                        int64_t i = idx.i64;
                        if (i >= 0 && static_cast<size_t>(i) < recv.str.size()) {
                            vals[inst.result] = CVal::makeStr(std::string(1, recv.str[static_cast<size_t>(i)]));
                        } else {
                            vals[inst.result] = CVal::makeUndef();
                        }
                        break;
                    }
                    std::string eKey = (idx.kind == CVal::Kind::String) ? idx.str : std::to_string(idx.i64);
                    vals[inst.result] = getProp(recv, eKey);
                    break;
                }
                case Op::ElemSet: case Op::ElemSetTyped: {
                    CVal recv = opV(0), idx = opV(1), val = opV(2);
                    std::string eKey = (idx.kind == CVal::Kind::String) ? idx.str : std::to_string(idx.i64);
                    setProp(recv, eKey, val);
                    if (inst.result != il::kNoValue) vals[inst.result] = val;
                    break;
                }
                case Op::NumericStep: vals[inst.result] = CVal::makeF64(opV(0).num + static_cast<double>(inst.immI32)); break;
                case Op::ConcatBegin:
                    vals[inst.result] = CVal::makeStr(inst.operands.empty() ? "" : (inst.operands.size() == 1 ? getStr(opV(0)) : (getStr(opV(0)) + getStr(opV(1)))));
                    break;
                case Op::ConcatAppend: vals[inst.result] = CVal::makeStr(getStr(opV(0)) + getStr(opV(1))); break;
                case Op::ConcatEnd: vals[inst.result] = CVal::makeStr(getStr(opV(0))); break;
                case Op::Add: {
                    CVal a = opV(0), b = opV(1);
                    if (a.kind == CVal::Kind::String || b.kind == CVal::Kind::String) {
                        vals[inst.result] = CVal::makeStr(getStr(a) + getStr(b));
                    } else {
                        vals[inst.result] = CVal::makeF64(a.num + b.num);
                    }
                    break;
                }
                case Op::Sub: vals[inst.result] = CVal::makeF64(opV(0).num - opV(1).num); break;
                case Op::Mul: vals[inst.result] = CVal::makeF64(opV(0).num * opV(1).num); break;
                case Op::Div: vals[inst.result] = CVal::makeF64(opV(0).num / opV(1).num); break;
                case Op::Mod: vals[inst.result] = CVal::makeF64(std::fmod(opV(0).num, opV(1).num)); break;
                case Op::Pow: vals[inst.result] = CVal::makeF64(std::pow(opV(0).num, opV(1).num)); break;
                case Op::Neg: vals[inst.result] = CVal::makeF64(-opV(0).num); break;
                case Op::MathImul: {
                    int32_t a = jsToInt32(opV(0).num), b = jsToInt32(opV(1).num);
                    vals[inst.result] = CVal::makeI64(static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b)));
                    break;
                }
                case Op::BitAnd: vals[inst.result] = CVal::makeI64(jsToInt32(opV(0).num) & jsToInt32(opV(1).num)); break;
                case Op::BitOr:  vals[inst.result] = CVal::makeI64(jsToInt32(opV(0).num) | jsToInt32(opV(1).num)); break;
                case Op::BitXor: vals[inst.result] = CVal::makeI64(jsToInt32(opV(0).num) ^ jsToInt32(opV(1).num)); break;
                case Op::BitNot: vals[inst.result] = CVal::makeI64(~jsToInt32(opV(0).num)); break;
                case Op::Shl: vals[inst.result] = CVal::makeI64(static_cast<int32_t>(static_cast<uint32_t>(jsToInt32(opV(0).num)) << (static_cast<uint32_t>(jsToInt32(opV(1).num)) & 31u))); break;
                case Op::Shr: vals[inst.result] = CVal::makeI64(jsToInt32(opV(0).num) >> (static_cast<uint32_t>(jsToInt32(opV(1).num)) & 31u)); break;
                case Op::UShr: vals[inst.result] = CVal::makeI64(static_cast<int64_t>(static_cast<uint32_t>(jsToInt32(opV(0).num)) >> (static_cast<uint32_t>(jsToInt32(opV(1).num)) & 31u))); break;
                case Op::CmpEq:
                case Op::StrictEq:
                case Op::LooseEq:
                case Op::CmpNe: {
                    CVal a = opV(0), b = opV(1);
                    bool isEqOp = (inst.op != Op::CmpNe);
                    bool eq = false;
                    if ((a.kind == CVal::Kind::Undefined || a.kind == CVal::Kind::Null) &&
                        (b.kind == CVal::Kind::Undefined || b.kind == CVal::Kind::Null)) {
                        eq = (inst.op == Op::LooseEq) ? true : (a.kind == b.kind);
                    } else if (a.kind == CVal::Kind::Undefined || b.kind == CVal::Kind::Undefined ||
                               a.kind == CVal::Kind::Null || b.kind == CVal::Kind::Null) {
                        eq = false;
                    } else if (a.kind == CVal::Kind::String && b.kind == CVal::Kind::String) {
                        eq = (a.str == b.str);
                    } else if (a.kind == CVal::Kind::Object && b.kind == CVal::Kind::Object) {
                        eq = (a.objId == b.objId);
                    } else {
                        eq = (a.num == b.num);
                    }
                    vals[inst.result] = CVal::makeBool(isEqOp ? eq : !eq);
                    break;
                }
                case Op::CmpLt: case Op::RelLt:
                case Op::CmpLe: case Op::RelLe:
                case Op::CmpGt: case Op::RelGt:
                case Op::CmpGe: case Op::RelGe: {
                    double a = opV(0).num, b = opV(1).num;
                    bool r = false;
                    if (!std::isnan(a) && !std::isnan(b)) {
                        if (inst.op == Op::CmpLt || inst.op == Op::RelLt) r = (a < b);
                        else if (inst.op == Op::CmpLe || inst.op == Op::RelLe) r = (a <= b);
                        else if (inst.op == Op::CmpGt || inst.op == Op::RelGt) r = (a > b);
                        else r = (a >= b);
                    }
                    vals[inst.result] = CVal::makeBool(r);
                    break;
                }
                case Op::MathUnary: {
                    double a = opV(0).num;
                    auto kind = static_cast<il::MathUnaryFn>(inst.immI32);
                    if (kind == il::MathUnaryFn::Abs) vals[inst.result] = CVal::makeF64(std::fabs(a));
                    else if (kind == il::MathUnaryFn::Floor) vals[inst.result] = CVal::makeF64(std::floor(a));
                    else if (kind == il::MathUnaryFn::Ceil) vals[inst.result] = CVal::makeF64(std::ceil(a));
                    else if (kind == il::MathUnaryFn::Trunc) vals[inst.result] = CVal::makeF64(std::trunc(a));
                    else if (kind == il::MathUnaryFn::Round) vals[inst.result] = CVal::makeF64(jsMathRound(a));
                    else if (kind == il::MathUnaryFn::Sqrt) vals[inst.result] = CVal::makeF64(std::sqrt(a));
                    else if (kind == il::MathUnaryFn::Min || kind == il::MathUnaryFn::Max) {
                        double acc = a;
                        for (size_t k = 1; k < inst.operands.size(); ++k) {
                            acc = jsMathMinMax(acc, opV(k).num, kind == il::MathUnaryFn::Min);
                        }
                        vals[inst.result] = CVal::makeF64(acc);
                    } else {
                        res.failReason = "unsupported MathUnary";
                        return res;
                    }
                    break;
                }
                case Op::GlobalGet:
                case Op::ResolveName: {
                    std::string gName = (inst.keyIndex < mod.keyConstants.size()) ? mod.keyConstants[inst.keyIndex] : "";
                    if (gName == "undefined") { vals[inst.result] = CVal::makeUndef(); break; }
                    if (gName == "NaN") { vals[inst.result] = CVal::makeF64(std::numeric_limits<double>::quiet_NaN()); break; }
                    if (gName == "Infinity") { vals[inst.result] = CVal::makeF64(std::numeric_limits<double>::infinity()); break; }
                    CVal gv = CVal::makeI64(1);
                    gv.globalName = gName;
                    for (size_t fi = 0; fi < mod.functions.size(); ++fi) {
                        const auto& cand = mod.functions[fi];
                        if (!cand.isExternal && !cand.isEntryPoint && (cand.name == gName || cand.displayName == gName)) {
                            gv.kind = CVal::Kind::Function;
                            gv.fnIdx = static_cast<uint32_t>(fi);
                            gv.envId = funcCapturedEnvs.count(gv.fnIdx) ? funcCapturedEnvs[gv.fnIdx] : 0;
                            break;
                        }
                    }
                    vals[inst.result] = gv;
                    break;
                }
                case Op::Construct: {
                    uint32_t oid = allocObj("__local_" + std::to_string(heap.size()));
                    CVal newObj = CVal::makeObj(oid, static_cast<int64_t>(0x00B1000000000000ULL + oid));
                    CVal calleeV = opV(0);
                    if (calleeV.kind == CVal::Kind::Function && calleeV.fnIdx < mod.functions.size()) {
                        heap[oid].ctorIdx = calleeV.fnIdx;
                        std::vector<CVal> cArgs;
                        for (size_t k = 1; k < inst.operands.size(); ++k) cArgs.push_back(opV(k));
                        CVal subRet;
                        auto sub = evalFn(mod.functions[calleeV.fnIdx], cArgs, newObj, calleeV.envId, depth + 1, true, &subRet);
                        if (!sub.completed || sub.threw) return sub;
                        vals[inst.result] = (subRet.kind == CVal::Kind::Object && subRet.objId > 0) ? subRet : newObj;
                        break;
                    }
                    if (calleeV.globalName.empty() && !inModuleInit) {
                        res.failReason = "unsupported construct on unknown target";
                        return res;
                    }
                    if (inst.operands.size() >= 2) setProp(newObj, "length", opV(1));
                    vals[inst.result] = newObj;
                    break;
                }
                case Op::SuperCall: vals[inst.result] = thisVal; break;
                case Op::MethodCall: {
                    std::vector<CVal> opVals;
                    for (size_t k = 0; k < inst.operands.size(); ++k) opVals.push_back(opV(k));
                    CVal outV;
                    if (!evalMethodCall(inst, opVals, depth, outV, res)) return res;
                    vals[inst.result] = outV;
                    break;
                }
                case Op::MethodCallSpread:
                case Op::DynamicCallSpread: {
                    bool isMethod = (inst.op == Op::MethodCallSpread);
                    CVal spreadArr = isMethod ? opV(1) : opV(2);
                    int64_t len = std::clamp<int64_t>(getProp(spreadArr, "length").i64, 0, 16);
                    std::string s;
                    for (int64_t i = 0; i < len; ++i) {
                        s.push_back(static_cast<char>(getProp(spreadArr, std::to_string(i)).i64 & 0x7F));
                    }
                    vals[inst.result] = CVal::makeStr(s);
                    break;
                }
                case Op::DynamicCall: {
                    std::vector<CVal> opVals;
                    for (size_t k = 0; k < inst.operands.size(); ++k) opVals.push_back(opV(k));
                    CVal outV;
                    if (!evalDynamicCall(inst, opVals, depth, outV, res)) return res;
                    vals[inst.result] = outV;
                    break;
                }
                case Op::Call: {
                    if (inst.calleeIndex >= mod.functions.size()) { res.failReason = "bad callee"; return res; }
                    const auto& callee = mod.functions[inst.calleeIndex];
                    uint32_t cEnv = 0;
                    if (callee.needsEnv && !inst.operands.empty()) {
                        uint32_t cur = opV(0).envId;
                        uint32_t hops = (inst.callEnvHops != il::Instruction::kNoEnvHops) ? inst.callEnvHops : 0;
                        for (uint32_t h = 0; h < hops; ++h) cur = envParent.count(cur) ? envParent[cur] : 0;
                        cEnv = cur;
                    }
                    CVal cThis = (callee.needsThis && (callee.needsEnv ? 1u : 0u) < inst.operands.size())
                                     ? opV(callee.needsEnv ? 1 : 0) : thisVal;
                    size_t startOp = (inst.operands.size() == callee.params.size())
                                         ? callee.firstSourceParam()
                                         : ((inst.callEnvHops != il::Instruction::kNoEnvHops && !inst.operands.empty()) ? 1 : 0);
                    std::vector<CVal> cArgs;
                    for (size_t k = startOp; k < inst.operands.size(); ++k) cArgs.push_back(opV(k));
                    CVal subRet;
                    auto sub = evalFn(callee, cArgs, cThis, cEnv, depth + 1, false, &subRet);
                    if (!sub.completed || sub.threw) return sub;
                    vals[inst.result] = subRet;
                    break;
                }
                case Op::EnvCreate: {
                    uint32_t pId = !inst.operands.empty() ? opV(0).envId : 0;
                    uint32_t nId = (depth == 0 && nextEnvId == 1 && fn.isEntryPoint) ? 0 : nextEnvId++;
                    if (nId != 0) envParent[nId] = pId;
                    CVal ev = CVal::makeI64(0);
                    ev.envId = nId;
                    vals[inst.result] = ev;
                    break;
                }
                case Op::ModuleEnvSet: break;
                case Op::ModuleEnvGet: vals[inst.result] = CVal::makeI64(0); break;
                case Op::EnvSet:
                    if (inst.operands.size() >= 2) {
                        envSlots[resolveEnvKey(opV(0).envId, inst.envDepth, inst.envIndex)] = opV(1);
                    }
                    break;
                case Op::EnvGet:
                case Op::EnvGetTdz: {
                    uint64_t k = resolveEnvKey(!inst.operands.empty() ? opV(0).envId : 0, inst.envDepth, inst.envIndex);
                    if (!envSlots.count(k)) {
                        for (uint32_t eid = 0; eid < nextEnvId; ++eid) {
                            uint64_t alt = (static_cast<uint64_t>(eid) << 32) | inst.envIndex;
                            if (envSlots.count(alt)) { k = alt; break; }
                        }
                    }
                    vals[inst.result] = envSlots.count(k) ? envSlots[k] : CVal::makeUndef();
                    break;
                }
                case Op::Throw:
                    res.completed = true;
                    res.threw = true;
                    return res;
                case Op::Jump:
                case Op::Branch: {
                    const il::BlockTarget& edge = (inst.op == Op::Jump || opV(0).isTruthy())
                                                      ? inst.target : inst.elseTarget;
                    if (edge.block < fn.blocks.size()) {
                        const auto& tb = fn.blocks[edge.block];
                        std::vector<std::pair<il::ValueId, CVal>> nextParams;
                        for (size_t k = 0; k < tb.params.size() && k < edge.args.size(); ++k) {
                            nextParams.emplace_back(tb.params[k].id, getV(edge.args[k]));
                        }
                        for (auto& [dst, v] : nextParams) vals[dst] = std::move(v);
                    }
                    blockId = edge.block;
                    jumped = true;
                    break;
                }
                case Op::Ret: {
                    CVal r = inst.operands.empty() ? CVal::makeUndef() : opV(0);
                    if (outRetVal) *outRetVal = r;
                    res.completed = true;
                    res.isUndefined = (r.kind == CVal::Kind::Undefined);
                    res.isString = (r.kind == CVal::Kind::String);
                    res.returnStr = r.str;
                    res.returnNum = r.num;
                    res.returnI64 = r.i64;
                    res.finalHeapWrites = recordedWrites;
                    if (!outRetVal && r.objId > 0 && r.objId < heap.size() &&
                        heap[r.objId].rootPath.rfind("__local_", 0) == 0) {
                        for (const auto& [k, pv] : heap[r.objId].props) {
                            if (pv.objId > 0 && pv.objId < heap.size() &&
                                heap[pv.objId].rootPath.rfind("__local_", 0) == 0) {
                                for (const auto& [sk, spv] : heap[pv.objId].props) {
                                    res.finalHeapWrites["__ret." + k + "." + sk] = spv.i64;
                                }
                                res.finalHeapWrites["__ret." + k] = static_cast<int64_t>(0x00B100000000FFFEULL);
                            } else if (pv.kind == CVal::Kind::Function) {
                                res.finalHeapWrites["__ret." + k] = static_cast<int64_t>(0x00B100000000FFFDULL);
                            } else {
                                res.finalHeapWrites["__ret." + k] = pv.i64;
                            }
                        }
                        res.returnI64 = static_cast<int64_t>(0x00B100000000FFFFULL);
                        res.returnNum = static_cast<double>(res.returnI64);
                    }
                    return res;
                }
                default:
                    if (inModuleInit) {
                        if (inst.result != il::kNoValue) vals[inst.result] = CVal::makeUndef();
                        break;
                    }
                    res.failReason = std::string("unsupported op: ") + il::opName(inst.op);
                    return res;
            }
            if (jumped) break;
        }
        if (!jumped) { res.failReason = "block fell through"; return res; }
    }
    res.failReason = "step limit exceeded";
    return res;
}

InterpResult runIlFunctionConcrete(const il::Module& mod,
                                   const il::Function& fn,
                                   const std::vector<std::string>& paramNames,
                                   const std::vector<int64_t>& argValues,
                                   uint32_t maxSteps,
                                   bool floatBitArgs) {
    ConcreteVM vm(mod, maxSteps);
    for (const auto& f : mod.functions) {
        if (!f.isExternal && (f.isEntryPoint || f.name == "main") && !f.blocks.empty()) {
            uint32_t savedSteps = vm.stepsLeft;
            vm.stepsLeft = 10000;
            vm.inModuleInit = true;
            vm.evalFn(f, {}, CVal::makeObj(vm.allocObj("this")), 0, 0);
            vm.inModuleInit = false;
            vm.stepsLeft = savedSteps;
            vm.recordedWrites.clear();
            break;
        }
    }

    uint32_t fnIdx = UINT32_MAX;
    for (size_t fi = 0; fi < mod.functions.size(); ++fi) {
        if (&mod.functions[fi] == &fn) { fnIdx = static_cast<uint32_t>(fi); break; }
    }
    uint32_t thisObjId = vm.allocObj("this");
    if (fn.needsThis && vm.moduleCtorIdx != UINT32_MAX && fnIdx != vm.moduleCtorIdx) {
        vm.heap[thisObjId].ctorIdx = vm.moduleCtorIdx;
    }
    CVal thisVal = CVal::makeObj(thisObjId, static_cast<int64_t>(0x00B1000000000001ULL));
    auto decodeArgVal = [&](int64_t raw) -> CVal {
        if (floatBitArgs) {
            double d = 0.0;
            std::memcpy(&d, &raw, sizeof(double));
            return CVal::makeF64(d);
        }
        return CVal::makeI64(raw);
    };

    uint32_t srcCount = static_cast<uint32_t>(fn.callerParamCount());
    std::vector<CVal> callArgs(srcCount);
    std::unordered_map<std::string, uint32_t> rootObjs;
    rootObjs["this"] = thisObjId;
    for (uint32_t i = 0; i < srcCount; ++i) {
        std::string rName = "arg_" + std::to_string(i);
        uint32_t oid = vm.allocObj(rName);
        if (fn.needsThis && vm.moduleCtorIdx != UINT32_MAX && fnIdx != vm.moduleCtorIdx) {
            vm.heap[oid].ctorIdx = vm.moduleCtorIdx;
        }
        rootObjs[rName] = oid;
        callArgs[i] = decodeArgVal((i < argValues.size()) ? argValues[i] : 0);
        callArgs[i].objId = oid;
    }

    for (size_t i = srcCount; i < paramNames.size() && i < argValues.size(); ++i) {
        const std::string& path = paramNames[i];
        auto dot = path.find('.');
        if (dot == std::string::npos) continue;
        auto it = rootObjs.find(path.substr(0, dot));
        if (it == rootObjs.end()) continue;
        std::string rest = path.substr(dot + 1);
        uint32_t curObj = it->second;
        while (true) {
            auto nextDot = rest.find('.');
            if (nextDot == std::string::npos) {
                if (!vm.heap[curObj].props.count(rest) || vm.heap[curObj].props[rest].objId == 0) {
                    vm.heap[curObj].props[rest] = decodeArgVal(argValues[i]);
                }
                break;
            }
            std::string seg = rest.substr(0, nextDot);
            rest = rest.substr(nextDot + 1);
            auto pIt = vm.heap[curObj].props.find(seg);
            if (pIt != vm.heap[curObj].props.end() && pIt->second.objId > 0) {
                curObj = pIt->second.objId;
            } else {
                uint32_t subId = vm.allocObj(vm.heap[curObj].rootPath + "." + seg);
                vm.heap[curObj].props[seg] = CVal::makeObj(subId);
                curObj = subId;
            }
        }
    }
    if (functionUsesStringOps(mod, fn)) {
        for (uint32_t i = 0; i < srcCount; ++i) {
            if (vm.heap[callArgs[i].objId].props.count("length")) {
                uint32_t oid = callArgs[i].objId;
                callArgs[i] = CVal::makeStr(vm.getStr(callArgs[i]));
                callArgs[i].objId = oid;
            }
        }
    }
    return vm.evalFn(fn, callArgs, thisVal, 0, 0);
}

bool concreteResultsDiverge(const InterpResult& oldRun,
                            const InterpResult& newRun,
                            int64_t* outOldScalar,
                            int64_t* outNewScalar) {
    if (oldRun.threw != newRun.threw) return true;
    if (oldRun.threw && newRun.threw) return false;
    if (oldRun.isUndefined != newRun.isUndefined) return true;
    if (oldRun.isString != newRun.isString) return true;
    if (oldRun.isString && oldRun.returnStr != newRun.returnStr) return true;

    bool oldNaN = std::isnan(oldRun.returnNum);
    bool newNaN = std::isnan(newRun.returnNum);
    if (oldNaN != newNaN) return true;
    bool signZeroDiff = (oldRun.returnNum == 0.0 && newRun.returnNum == 0.0 &&
                         std::signbit(oldRun.returnNum) != std::signbit(newRun.returnNum));
    if (!oldNaN && (oldRun.returnI64 != newRun.returnI64 || oldRun.returnNum != newRun.returnNum || signZeroDiff)) {
        if (outOldScalar) *outOldScalar = oldRun.returnI64;
        if (outNewScalar) *outNewScalar = newRun.returnI64;
        return true;
    }

    for (const auto& [k, ov] : oldRun.finalHeapWrites) {
        auto it = newRun.finalHeapWrites.find(k);
        if (it != newRun.finalHeapWrites.end() && it->second != ov) {
            if (outOldScalar) *outOldScalar = ov;
            if (outNewScalar) *outNewScalar = it->second;
            return true;
        }
    }
    return false;
}

bool verifyCounterexampleWithReplay(const il::Module& oldMod,
                                    const il::Function& oldFn,
                                    const il::Module& newMod,
                                    const il::Function& newFn,
                                    FunctionEquivResult& res,
                                    bool floatBitArgs) {
    if (res.status != EquivStatus::Counterexample) return true;
    InterpResult oldRun = runIlFunctionConcrete(oldMod, oldFn, res.paramNames, res.counterexampleArgs, 50000, floatBitArgs);
    InterpResult newRun = runIlFunctionConcrete(newMod, newFn, res.paramNames, res.counterexampleArgs, 50000, floatBitArgs);
    if (!oldRun.completed || !newRun.completed) {
        if (oldRun.failReason == "step limit exceeded" || newRun.failReason == "step limit exceeded") {
            return true;
        }
        res.status = EquivStatus::Unknown;
        res.detail = "prover/replay mismatch";
        res.counterexampleArgs.clear();
        return false;
    }
    int64_t replayOld = res.oldReturnValue;
    int64_t replayNew = res.newReturnValue;
    if (!concreteResultsDiverge(oldRun, newRun, &replayOld, &replayNew)) {
        res.status = EquivStatus::Unknown;
        res.detail = "prover/replay mismatch";
        res.counterexampleArgs.clear();
        return false;
    }
    if (res.oldReturnValue == res.newReturnValue) {
        res.oldReturnValue = replayOld;
        res.newReturnValue = replayNew;
    }
    res.oldThrows = oldRun.threw;
    res.newThrows = newRun.threw;
    return true;
}

}  // namespace bronze::check
