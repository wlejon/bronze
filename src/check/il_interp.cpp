#include "check/il_interp.h"
#include "check/prove_equiv_heap.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace bronze::check {
namespace {

int32_t jsToInt32(double d) {
    if (!std::isfinite(d) || d == 0.0) return 0;
    double intPart = std::trunc(d);
    double mod2_32 = std::fmod(intPart, 4294967296.0);
    if (mod2_32 < 0.0) mod2_32 += 4294967296.0;
    uint32_t u32 = static_cast<uint32_t>(mod2_32);
    return static_cast<int32_t>(u32);
}

double jsMathRound(double a) {
    if (std::isnan(a) || !std::isfinite(a) || a == 0.0) return a;
    if (a < 0.0 && a >= -0.5) return -0.0;
    return std::floor(a + 0.5);
}

double jsMathMinMax(double a, double b, bool isMin) {
    if (std::isnan(a) || std::isnan(b)) return std::numeric_limits<double>::quiet_NaN();
    if (a == 0.0 && b == 0.0) {
        return isMin ? ((std::signbit(a) || std::signbit(b)) ? -0.0 : 0.0)
                     : ((std::signbit(a) && std::signbit(b)) ? -0.0 : 0.0);
    }
    return isMin ? std::min(a, b) : std::max(a, b);
}

struct CVal {
    enum class Kind : uint8_t { Undefined, Null, Bool, Number, String, Object, Function };
    Kind kind = Kind::Number;
    double num = 0.0;
    int64_t i64 = 0;
    std::string str;
    uint32_t objId = 0;
    uint32_t fnIdx = 0;
    uint32_t envId = 0;
    std::string globalName;

    static CVal makeUndef() { CVal v; v.kind = Kind::Undefined; return v; }
    static CVal makeNull() { CVal v; v.kind = Kind::Null; return v; }
    static CVal makeBool(bool b) { CVal v; v.kind = Kind::Bool; v.num = b ? 1.0 : 0.0; v.i64 = b ? 1 : 0; return v; }
    static CVal makeI64(int64_t x) { CVal v; v.kind = Kind::Number; v.i64 = x; v.num = static_cast<double>(x); return v; }
    static CVal makeF64(double d) {
        CVal v;
        v.kind = Kind::Number;
        v.num = d;
        v.i64 = (std::isfinite(d) && d >= static_cast<double>(INT64_MIN) && d < static_cast<double>(INT64_MAX))
                    ? static_cast<int64_t>(d) : 0;
        return v;
    }
    static CVal makeStr(std::string s) {
        CVal v;
        v.kind = Kind::String;
        v.i64 = static_cast<int64_t>(packConcreteString64(s));
        v.num = static_cast<double>(v.i64);
        v.str = std::move(s);
        return v;
    }
    static CVal makeObj(uint32_t id, int64_t scalarFallback = 0) {
        CVal v;
        v.kind = Kind::Object;
        v.objId = id;
        v.i64 = scalarFallback;
        v.num = static_cast<double>(scalarFallback);
        return v;
    }

    bool isTruthy() const {
        switch (kind) {
            case Kind::Undefined:
            case Kind::Null: return false;
            case Kind::Bool: return i64 != 0;
            case Kind::Number: return !std::isnan(num) && num != 0.0;
            case Kind::String: return !str.empty();
            case Kind::Object:
            case Kind::Function: return true;
        }
        return false;
    }
};

struct CObject {
    std::string rootPath;
    std::map<std::string, CVal> props;
    std::vector<CVal> elems;
    std::map<std::string, uint32_t> methods;
    uint32_t ctorIdx = UINT32_MAX;
};

struct ConcreteVM {
    const il::Module& mod;
    uint32_t stepsLeft = 50000;
    std::vector<CObject> heap;
    std::unordered_map<uint64_t, CVal> envSlots;
    std::unordered_map<uint32_t, uint32_t> envParent;
    uint32_t nextEnvId = 1;
    std::map<std::string, int64_t> recordedWrites;

    explicit ConcreteVM(const il::Module& m, uint32_t maxSteps) : mod(m), stepsLeft(maxSteps) {
        heap.push_back({"__null", {}, {}, {}, UINT32_MAX});
    }

    uint32_t allocObj(std::string path) {
        uint32_t id = static_cast<uint32_t>(heap.size());
        heap.push_back({std::move(path), {}, {}, {}, UINT32_MAX});
        return id;
    }

    uint64_t resolveEnvKey(uint32_t curEnv, uint32_t depth, uint32_t index) const {
        for (uint32_t d = 0; d < depth; ++d) {
            auto it = envParent.find(curEnv);
            curEnv = (it != envParent.end()) ? it->second : 0;
        }
        return (static_cast<uint64_t>(curEnv) << 32) | index;
    }

    std::string getStr(const CVal& v) const {
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

    CVal getProp(const CVal& recv, const std::string& key) {
        if (recv.kind == CVal::Kind::String && key == "length") {
            return CVal::makeI64(static_cast<int64_t>(recv.str.size()));
        }
        if (recv.objId == 0 || recv.objId >= heap.size()) return CVal::makeUndef();
        const auto& obj = heap[recv.objId];
        auto it = obj.props.find(key);
        if (it != obj.props.end()) return it->second;
        if (key == "length") {
            if (!obj.elems.empty()) return CVal::makeI64(static_cast<int64_t>(obj.elems.size()));
            if (obj.rootPath.rfind("__local_", 0) != 0) return CVal::makeI64(1);
            return CVal::makeI64(0);
        }
        if (key == "_isBigNumber" || key == "isBigNumber" || key == "isDecimal") return CVal::makeI64(1);
        if (key == "sigBytes") return CVal::makeI64(4);
        if (obj.rootPath.rfind("__local_", 0) == 0) return CVal::makeUndef();
        CVal def = CVal::makeI64(0);
        if (!obj.rootPath.empty() && obj.rootPath.rfind("__fn_", 0) != 0) {
            uint32_t subId = allocObj(obj.rootPath + "." + key);
            def.objId = subId;
            heap[recv.objId].props[key] = def;
        }
        return def;
    }

    void setProp(const CVal& recv, const std::string& key, const CVal& val) {
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

    InterpResult evalFn(const il::Function& fn,
                        const std::vector<CVal>& args,
                        CVal thisVal,
                        uint32_t callerEnvId,
                        uint32_t depth,
                        bool isConstruct = false) {
        InterpResult res;
        if (fn.blocks.empty() || fn.isExternal || depth > 12) {
            res.failReason = "external or deep recursion";
            return res;
        }
        std::unordered_map<il::ValueId, CVal> vals;
        auto getV = [&](il::ValueId id) -> CVal {
            auto it = vals.find(id);
            return (it != vals.end()) ? it->second : CVal::makeI64(0);
        };
        uint32_t firstSrc = static_cast<uint32_t>(fn.firstSourceParam());
        if (fn.needsEnv) {
            CVal envV = CVal::makeI64(0);
            envV.envId = callerEnvId;
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
                        if (l.objId > 0 && l.objId < heap.size() && r.kind == CVal::Kind::Function && heap[l.objId].ctorIdx != UINT32_MAX) {
                            vals[inst.result] = CVal::makeBool(heap[l.objId].ctorIdx == r.fnIdx);
                            break;
                        }
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
                    case Op::CreateFunction: case Op::FunctionRef: {
                        CVal fv = CVal::makeObj(allocObj("__fn_" + std::to_string(inst.calleeIndex)));
                        fv.kind = CVal::Kind::Function;
                        fv.fnIdx = inst.calleeIndex;
                        fv.envId = (inst.op == Op::CreateFunction && !inst.operands.empty()) ? opV(0).envId : 0;
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
                        CVal val = opV(1);
                        setProp(opV(0), mod.keyConstants[inst.keyIndex], val);
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
                        vals[inst.result] = getProp(recv, std::to_string(idx.i64));
                        break;
                    }
                    case Op::ElemSet: case Op::ElemSetTyped: {
                        CVal recv = opV(0), idx = opV(1), val = opV(2);
                        setProp(recv, std::to_string(idx.i64), val);
                        if (inst.result != il::kNoValue) vals[inst.result] = val;
                        break;
                    }
                    case Op::NumericStep: vals[inst.result] = CVal::makeI64(opV(0).i64 + inst.immI32); break;
                    case Op::ConcatBegin: vals[inst.result] = CVal::makeStr(""); break;
                    case Op::ConcatAppend: vals[inst.result] = CVal::makeStr(getStr(opV(0)) + getStr(opV(1))); break;
                    case Op::ConcatEnd: vals[inst.result] = CVal::makeStr(getStr(opV(0))); break;
                    case Op::Add: {
                        CVal a = opV(0), b = opV(1);
                        bool hasNegZero = (a.num == 0.0 && std::signbit(a.num)) || (b.num == 0.0 && std::signbit(b.num));
                        if (a.kind == CVal::Kind::String || b.kind == CVal::Kind::String) {
                            vals[inst.result] = CVal::makeStr(getStr(a) + getStr(b));
                        } else if (!hasNegZero && std::trunc(a.num) == a.num && std::trunc(b.num) == b.num && std::isfinite(a.num) && std::isfinite(b.num)) {
                            vals[inst.result] = CVal::makeI64(a.i64 + b.i64);
                        } else {
                            vals[inst.result] = CVal::makeF64(a.num + b.num);
                        }
                        break;
                    }
                    case Op::Sub: {
                        CVal a = opV(0), b = opV(1);
                        bool hasNegZero = (a.num == 0.0 && std::signbit(a.num)) || (b.num == 0.0 && std::signbit(b.num));
                        vals[inst.result] = (!hasNegZero && std::trunc(a.num) == a.num && std::trunc(b.num) == b.num && std::isfinite(a.num) && std::isfinite(b.num))
                                                ? CVal::makeI64(a.i64 - b.i64) : CVal::makeF64(a.num - b.num);
                        break;
                    }
                    case Op::Mul: {
                        CVal a = opV(0), b = opV(1);
                        vals[inst.result] = (a.num != 0.0 && b.num != 0.0 && std::trunc(a.num) == a.num && std::trunc(b.num) == b.num && std::isfinite(a.num) && std::isfinite(b.num))
                                                ? CVal::makeI64(a.i64 * b.i64) : CVal::makeF64(a.num * b.num);
                        break;
                    }
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
                            auto sub = evalFn(mod.functions[calleeV.fnIdx], cArgs, newObj, calleeV.envId, depth + 1, true);
                            if (!sub.completed) return sub;
                            if (sub.threw) return sub;
                            vals[inst.result] = newObj;
                            break;
                        }
                        if (calleeV.globalName.empty()) {
                            res.failReason = "unsupported construct on unknown target";
                            return res;
                        }
                        if (inst.operands.size() >= 2) setProp(newObj, "length", opV(1));
                        vals[inst.result] = newObj;
                        break;
                    }
                    case Op::SuperCall:
                        vals[inst.result] = thisVal;
                        break;
                    case Op::MethodCall: {
                        if (inst.keyIndex >= mod.keyConstants.size()) { res.failReason = "bad method"; return res; }
                        const std::string& m = mod.keyConstants[inst.keyIndex];
                        CVal recv = opV(0);
                        if (recv.globalName == "Math") {
                            double a = opV(1).num, b = opV(2).num;
                            if (m == "abs") { vals[inst.result] = CVal::makeF64(std::fabs(a)); break; }
                            if (m == "floor") { vals[inst.result] = CVal::makeF64(std::floor(a)); break; }
                            if (m == "ceil") { vals[inst.result] = CVal::makeF64(std::ceil(a)); break; }
                            if (m == "trunc") { vals[inst.result] = CVal::makeF64(std::trunc(a)); break; }
                            if (m == "round") { vals[inst.result] = CVal::makeF64(jsMathRound(a)); break; }
                            if (m == "fround") { vals[inst.result] = CVal::makeF64(static_cast<double>(static_cast<float>(a))); break; }
                            if (m == "sqrt") { vals[inst.result] = CVal::makeF64(std::sqrt(a)); break; }
                            if (m == "imul") {
                                int32_t x = jsToInt32(a), y = jsToInt32(b);
                                vals[inst.result] = CVal::makeI64(static_cast<int32_t>(static_cast<uint32_t>(x) * static_cast<uint32_t>(y)));
                                break;
                            }
                            if (m == "clz32") {
                                uint32_t u = static_cast<uint32_t>(jsToInt32(a));
                                int lz = 32;
                                for (int bit = 31; bit >= 0; --bit) if ((u >> bit) & 1u) { lz = 31 - bit; break; }
                                vals[inst.result] = CVal::makeI64(lz);
                                break;
                            }
                            if (m == "sign") {
                                double s = std::isnan(a) ? a : (a == 0.0 ? a : (a < 0.0 ? -1.0 : 1.0));
                                vals[inst.result] = CVal::makeF64(s);
                                break;
                            }
                            if (m == "pow") { vals[inst.result] = CVal::makeF64(std::pow(a, b)); break; }
                            if (m == "min" || m == "max") {
                                double acc = a;
                                for (size_t k = 2; k < inst.operands.size(); ++k) {
                                    acc = jsMathMinMax(acc, opV(k).num, m == "min");
                                }
                                vals[inst.result] = CVal::makeF64(acc);
                                break;
                            }
                        }
                        if (recv.globalName == "Object" && m == "is") {
                            double a = opV(1).num, b = opV(2).num;
                            bool same = (std::isnan(a) && std::isnan(b)) || (a == b && std::signbit(a) == std::signbit(b));
                            vals[inst.result] = CVal::makeBool(same);
                            break;
                        }
                        if (m == "isNaN" && inst.operands.size() >= 2) {
                            vals[inst.result] = CVal::makeBool(std::isnan(opV(1).num));
                            break;
                        }
                        if (m == "isFinite" && inst.operands.size() >= 2) {
                            vals[inst.result] = CVal::makeBool(std::isfinite(opV(1).num));
                            break;
                        }
                        if (recv.globalName == "Number") {
                            double a = opV(1).num;
                            if (m == "isNaN") { vals[inst.result] = CVal::makeBool(std::isnan(a)); break; }
                            if (m == "isFinite") { vals[inst.result] = CVal::makeBool(std::isfinite(a)); break; }
                            if (m == "isInteger") { vals[inst.result] = CVal::makeBool(std::isfinite(a) && std::trunc(a) == a); break; }
                        }
                        if (recv.globalName == "String" && (m == "fromCharCode" || m == "fromCodePoint")) {
                            std::string s;
                            for (size_t k = 1; k < inst.operands.size(); ++k) {
                                s.push_back(static_cast<char>(jsToInt32(opV(k).num) & 0x7F));
                            }
                            vals[inst.result] = CVal::makeStr(s);
                            break;
                        }
                        if ((m == "charCodeAt" || m == "codePointAt") && inst.operands.size() >= 2) {
                            int64_t idx = opV(1).i64;
                            std::string s = getStr(recv);
                            if (idx >= 0 && static_cast<size_t>(idx) < s.size()) {
                                vals[inst.result] = CVal::makeI64(static_cast<unsigned char>(s[static_cast<size_t>(idx)]));
                            } else {
                                vals[inst.result] = CVal::makeI64(getProp(recv, std::to_string(idx)).i64 & 0xFFFF);
                            }
                            break;
                        }
                        if (m == "charAt" && inst.operands.size() >= 2) {
                            int64_t idx = opV(1).i64;
                            std::string s = getStr(recv);
                            vals[inst.result] = CVal::makeStr((idx >= 0 && static_cast<size_t>(idx) < s.size())
                                                                  ? std::string(1, s[static_cast<size_t>(idx)]) : "");
                            break;
                        }
                        if (m == "toLowerCase" || m == "toUpperCase") {
                            std::string s = getStr(recv);
                            for (char& ch : s) {
                                if (m == "toLowerCase" && ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + 32);
                                if (m == "toUpperCase" && ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 32);
                            }
                            vals[inst.result] = CVal::makeStr(s);
                            break;
                        }
                        if (m == "substring" && inst.operands.size() >= 2) {
                            std::string s = getStr(recv);
                            int64_t len = static_cast<int64_t>(s.size());
                            int64_t a = std::clamp<int64_t>(opV(1).i64, 0, len);
                            int64_t b = (inst.operands.size() >= 3) ? std::clamp<int64_t>(opV(2).i64, 0, len) : len;
                            if (a > b) std::swap(a, b);
                            vals[inst.result] = CVal::makeStr(s.substr(static_cast<size_t>(a), static_cast<size_t>(b - a)));
                            break;
                        }
                        if (m == "indexOf" && inst.operands.size() >= 2) {
                            CVal needle = opV(1);
                            if (recv.kind == CVal::Kind::String || needle.kind == CVal::Kind::String) {
                                std::string h = getStr(recv), nd = getStr(needle);
                                auto pos = h.find(nd);
                                vals[inst.result] = CVal::makeI64(pos == std::string::npos ? -1 : static_cast<int64_t>(pos));
                            } else {
                                int64_t len = getProp(recv, "length").i64, found = -1;
                                for (int64_t i = 0; i < len; ++i) {
                                    if (getProp(recv, std::to_string(i)).i64 == needle.i64) { found = i; break; }
                                }
                                vals[inst.result] = CVal::makeI64(found);
                            }
                            break;
                        }
                        if (m == "concat" && inst.operands.size() >= 2) {
                            vals[inst.result] = CVal::makeStr(getStr(recv) + getStr(opV(1)));
                            break;
                        }
                        if (m == "push" && inst.operands.size() >= 2) {
                            int64_t len = getProp(recv, "length").i64;
                            setProp(recv, std::to_string(len), opV(1));
                            setProp(recv, "length", CVal::makeI64(len + 1));
                            vals[inst.result] = CVal::makeI64(len + 1);
                            break;
                        }
                        if (m == "pop") {
                            int64_t len = getProp(recv, "length").i64;
                            CVal elem = (len > 0) ? getProp(recv, std::to_string(len - 1)) : CVal::makeI64(0);
                            setProp(recv, "length", CVal::makeI64(std::max<int64_t>(0, len - 1)));
                            vals[inst.result] = elem;
                            break;
                        }
                        if (m == "shift") {
                            int64_t len = getProp(recv, "length").i64;
                            CVal first = (len > 0) ? getProp(recv, "0") : CVal::makeI64(0);
                            for (int64_t i = 0; i + 1 < len; ++i) setProp(recv, std::to_string(i), getProp(recv, std::to_string(i + 1)));
                            setProp(recv, "length", CVal::makeI64(std::max<int64_t>(0, len - 1)));
                            vals[inst.result] = first;
                            break;
                        }
                        if (m == "unshift" && inst.operands.size() >= 2) {
                            int64_t len = getProp(recv, "length").i64;
                            for (int64_t i = len; i >= 1; --i) setProp(recv, std::to_string(i), getProp(recv, std::to_string(i - 1)));
                            setProp(recv, "0", opV(1));
                            setProp(recv, "length", CVal::makeI64(len + 1));
                            vals[inst.result] = CVal::makeI64(len + 1);
                            break;
                        }
                        if (m == "join") {
                            int64_t len = std::clamp<int64_t>(getProp(recv, "length").i64, 0, 8);
                            int64_t sep = (inst.operands.size() >= 2 && !opV(1).str.empty())
                                              ? static_cast<unsigned char>(opV(1).str[0]) : 44;
                            uint64_t acc = static_cast<uint64_t>(len) & 0xFULL;
                            for (int64_t i = 0; i < len; ++i) {
                                acc = (acc * 131ULL) + static_cast<uint64_t>(getProp(recv, std::to_string(i)).i64) +
                                      (i > 0 ? static_cast<uint64_t>(sep) : 0ULL);
                            }
                            vals[inst.result] = CVal::makeI64(static_cast<int64_t>(acc));
                            break;
                        }
                        if (m == "slice" || m == "subarray") {
                            if (recv.kind == CVal::Kind::String) {
                                std::string s = getStr(recv);
                                int64_t len = static_cast<int64_t>(s.size());
                                int64_t st = (inst.operands.size() >= 2) ? opV(1).i64 : 0;
                                int64_t en = (inst.operands.size() >= 3) ? opV(2).i64 : len;
                                if (st < 0) st = std::max<int64_t>(0, len + st); else st = std::min(st, len);
                                if (en < 0) en = std::max<int64_t>(0, len + en); else en = std::min(en, len);
                                vals[inst.result] = CVal::makeStr(en > st ? s.substr(static_cast<size_t>(st), static_cast<size_t>(en - st)) : "");
                                break;
                            }
                            int64_t len = getProp(recv, "length").i64;
                            int64_t st = (inst.operands.size() >= 2) ? opV(1).i64 : 0;
                            int64_t en = (inst.operands.size() >= 3) ? opV(2).i64 : len;
                            if (st < 0) st = std::max<int64_t>(0, len + st); else st = std::min(st, len);
                            if (en < 0) en = std::max<int64_t>(0, len + en); else en = std::min(en, len);
                            int64_t nl = std::max<int64_t>(0, en - st);
                            uint32_t oid = allocObj("__local_" + std::to_string(heap.size()));
                            CVal nv = CVal::makeObj(oid, static_cast<int64_t>(0x00B1000000000000ULL + oid));
                            setProp(nv, "length", CVal::makeI64(nl));
                            for (int64_t i = 0; i < nl; ++i) setProp(nv, std::to_string(i), getProp(recv, std::to_string(st + i)));
                            vals[inst.result] = nv;
                            break;
                        }
                        const il::Function* callee = nullptr;
                        if (inst.directTarget != il::Instruction::kNoDirectTarget && inst.directTarget < mod.functions.size()) {
                            callee = &mod.functions[inst.directTarget];
                        } else if (recv.objId > 0 && recv.objId < heap.size() && heap[recv.objId].methods.count(m)) {
                            uint32_t fi = heap[recv.objId].methods[m];
                            if (fi < mod.functions.size()) callee = &mod.functions[fi];
                        } else {
                            SymbolicHeapState dummyHeap;
                            callee = findMethodCalleeInModule(mod, dummyHeap, "", m);
                        }
                        if (callee && !callee->isExternal) {
                            std::vector<CVal> cArgs;
                            for (size_t k = 1; k < inst.operands.size(); ++k) cArgs.push_back(opV(k));
                            auto sub = evalFn(*callee, cArgs, recv, 0, depth + 1);
                            if (!sub.completed || sub.threw) return sub;
                            vals[inst.result] = sub.isUndefined ? CVal::makeUndef()
                                              : (sub.isString ? CVal::makeStr(sub.returnStr) : CVal::makeF64(sub.returnNum));
                            vals[inst.result].i64 = sub.returnI64;
                            break;
                        }
                        res.failReason = "unsupported method: " + m;
                        return res;
                    }
                    case Op::DynamicCall: {
                        CVal calleeV = opV(0);
                        if (calleeV.globalName == "isNaN" && inst.operands.size() >= 3) {
                            vals[inst.result] = CVal::makeBool(std::isnan(opV(2).num));
                            break;
                        }
                        if (calleeV.globalName == "isFinite" && inst.operands.size() >= 3) {
                            vals[inst.result] = CVal::makeBool(std::isfinite(opV(2).num));
                            break;
                        }
                        if (calleeV.globalName == "Error" || calleeV.globalName == "RangeError" ||
                            calleeV.globalName == "TypeError" || calleeV.globalName == "SyntaxError") {
                            vals[inst.result] = CVal::makeObj(allocObj("__err"));
                            break;
                        }
                        if (calleeV.kind == CVal::Kind::Function && calleeV.fnIdx < mod.functions.size()) {
                            std::vector<CVal> cArgs;
                            for (size_t k = 2; k < inst.operands.size(); ++k) cArgs.push_back(opV(k));
                            auto sub = evalFn(mod.functions[calleeV.fnIdx], cArgs, opV(1), calleeV.envId, depth + 1);
                            if (!sub.completed || sub.threw) return sub;
                            vals[inst.result] = CVal::makeF64(sub.returnNum);
                            vals[inst.result].i64 = sub.returnI64;
                            break;
                        }
                        res.failReason = "unsupported dynamic call";
                        return res;
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
                        auto sub = evalFn(callee, cArgs, cThis, cEnv, depth + 1);
                        if (!sub.completed || sub.threw) return sub;
                        vals[inst.result] = CVal::makeF64(sub.returnNum);
                        vals[inst.result].i64 = sub.returnI64;
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
                        res.completed = true;
                        res.isUndefined = (r.kind == CVal::Kind::Undefined);
                        res.isString = (r.kind == CVal::Kind::String);
                        res.returnStr = r.str;
                        res.returnNum = r.num;
                        res.returnI64 = r.i64;
                        res.finalHeapWrites = recordedWrites;
                        if (r.objId > 0 && r.objId < heap.size() && heap[r.objId].rootPath.rfind("__local_", 0) == 0) {
                            for (const auto& [k, pv] : heap[r.objId].props) res.finalHeapWrites["__ret." + k] = pv.i64;
                            res.returnI64 = static_cast<int64_t>(0x00B100000000FFFFULL);
                            res.returnNum = static_cast<double>(res.returnI64);
                        }
                        return res;
                    }
                    default:
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
};

}  // namespace

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
            vm.evalFn(f, {}, CVal::makeObj(vm.allocObj("this")), 0, 0);
            vm.stepsLeft = savedSteps;
            vm.recordedWrites.clear();
            break;
        }
    }

    uint32_t thisObjId = vm.allocObj("this");
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
                vm.heap[curObj].props[rest] = decodeArgVal(argValues[i]);
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
        return true;
    }
    if (!concreteResultsDiverge(oldRun, newRun)) {
        res.status = EquivStatus::Unknown;
        res.detail = "prover/replay mismatch";
        res.counterexampleArgs.clear();
        return false;
    }
    res.oldThrows = oldRun.threw;
    res.newThrows = newRun.threw;
    return true;
}

}  // namespace bronze::check
