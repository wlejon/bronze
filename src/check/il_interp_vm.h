#pragma once

#include "check/il_interp.h"
#include "check/prove_equiv_heap.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace bronze::check {

inline int32_t jsToInt32(double d) {
    if (!std::isfinite(d) || d == 0.0) return 0;
    double intPart = std::trunc(d);
    double mod2_32 = std::fmod(intPart, 4294967296.0);
    if (mod2_32 < 0.0) mod2_32 += 4294967296.0;
    uint32_t u32 = static_cast<uint32_t>(mod2_32);
    return static_cast<int32_t>(u32);
}

inline double jsMathRound(double a) {
    if (std::isnan(a) || !std::isfinite(a) || a == 0.0) return a;
    if (a < 0.0 && a >= -0.5) return -0.0;
    return std::floor(a + 0.5);
}

inline double jsMathMinMax(double a, double b, bool isMin) {
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
    bool inModuleInit = false;
    std::vector<CObject> heap;
    std::unordered_map<uint64_t, CVal> envSlots;
    std::unordered_map<uint32_t, uint32_t> envParent;
    std::unordered_map<uint32_t, uint32_t> funcCapturedEnvs;
    uint32_t moduleCtorIdx = UINT32_MAX;
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

    std::string getStr(const CVal& v) const;
    CVal getProp(const CVal& recv, const std::string& key);
    void setProp(const CVal& recv, const std::string& key, const CVal& val);

    bool evalMethodCall(const il::Instruction& inst,
                        const std::vector<CVal>& opVals,
                        uint32_t depth,
                        CVal& outVal,
                        InterpResult& res);

    bool evalDynamicCall(const il::Instruction& inst,
                         const std::vector<CVal>& opVals,
                         uint32_t depth,
                         CVal& outVal,
                         InterpResult& res);

    InterpResult evalFn(const il::Function& fn,
                        const std::vector<CVal>& args,
                        CVal thisVal,
                        uint32_t callerEnvId,
                        uint32_t depth,
                        bool isConstruct = false,
                        CVal* outRetVal = nullptr);
};

}  // namespace bronze::check
