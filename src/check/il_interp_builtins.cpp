#include "check/il_interp_vm.h"

namespace bronze::check {

namespace {

bool evalMathMethodConcrete(const std::string& m,
                            const std::vector<CVal>& args,
                            CVal& outVal) {
    double a = !args.empty() ? args[0].num : 0.0;
    double b = (args.size() >= 2) ? args[1].num : 0.0;
    if (m == "abs") { outVal = CVal::makeF64(std::fabs(a)); return true; }
    if (m == "floor") { outVal = CVal::makeF64(std::floor(a)); return true; }
    if (m == "ceil") { outVal = CVal::makeF64(std::ceil(a)); return true; }
    if (m == "trunc") { outVal = CVal::makeF64(std::trunc(a)); return true; }
    if (m == "round") { outVal = CVal::makeF64(jsMathRound(a)); return true; }
    if (m == "fround") { outVal = CVal::makeF64(static_cast<double>(static_cast<float>(a))); return true; }
    if (m == "sqrt") { outVal = CVal::makeF64(std::sqrt(a)); return true; }
    if (m == "imul") {
        int32_t x = jsToInt32(a), y = jsToInt32(b);
        outVal = CVal::makeI64(static_cast<int32_t>(static_cast<uint32_t>(x) * static_cast<uint32_t>(y)));
        return true;
    }
    if (m == "clz32") {
        uint32_t u = static_cast<uint32_t>(jsToInt32(a));
        int lz = 32;
        for (int bit = 31; bit >= 0; --bit) if ((u >> bit) & 1u) { lz = 31 - bit; break; }
        outVal = CVal::makeI64(lz);
        return true;
    }
    if (m == "log2") {
        outVal = CVal::makeF64(std::floor(std::log2(a)));
        return true;
    }
    if (m == "sign") {
        double s = std::isnan(a) ? a : (a == 0.0 ? a : (a < 0.0 ? -1.0 : 1.0));
        outVal = CVal::makeF64(s);
        return true;
    }
    if (m == "pow") { outVal = CVal::makeF64(std::pow(a, b)); return true; }
    if (m == "min" || m == "max") {
        double acc = a;
        for (size_t k = 1; k < args.size(); ++k) {
            acc = jsMathMinMax(acc, args[k].num, m == "min");
        }
        outVal = CVal::makeF64(acc);
        return true;
    }
    return false;
}

}  // namespace

bool ConcreteVM::evalMethodCall(const il::Instruction& inst,
                                const std::vector<CVal>& opVals,
                                uint32_t depth,
                                CVal& outVal,
                                InterpResult& res) {
    if (inst.keyIndex >= mod.keyConstants.size()) {
        res.failReason = "bad method";
        return false;
    }
    const std::string& m = mod.keyConstants[inst.keyIndex];
    auto opV = [&](size_t k) { return (k < opVals.size()) ? opVals[k] : CVal::makeUndef(); };
    CVal recv = opV(0);
    std::vector<CVal> mArgs;
    for (size_t k = 1; k < opVals.size(); ++k) mArgs.push_back(opVals[k]);

    if (recv.globalName == "Math" && evalMathMethodConcrete(m, mArgs, outVal)) {
        return true;
    }
    if (recv.globalName == "Object") {
        if (m == "is") {
            double a = opV(1).num, b = opV(2).num;
            bool same = (std::isnan(a) && std::isnan(b)) || (a == b && std::signbit(a) == std::signbit(b));
            outVal = CVal::makeBool(same);
            return true;
        }
        if (m == "setPrototypeOf") { outVal = opV(1); return true; }
        if (m == "create") { outVal = CVal::makeObj(allocObj("__local_" + std::to_string(heap.size()))); return true; }
    }
    if (recv.globalName == "Array" && m == "isArray") {
        outVal = CVal::makeBool(true);
        return true;
    }
    if (m == "isNaN" && opVals.size() >= 2) {
        outVal = CVal::makeBool(std::isnan(opV(1).num));
        return true;
    }
    if (m == "isFinite" && opVals.size() >= 2) {
        outVal = CVal::makeBool(std::isfinite(opV(1).num));
        return true;
    }
    if (recv.globalName == "Number") {
        double a = opV(1).num;
        if (m == "isNaN") { outVal = CVal::makeBool(std::isnan(a)); return true; }
        if (m == "isFinite") { outVal = CVal::makeBool(std::isfinite(a)); return true; }
        if (m == "isInteger" || m == "isSafeInteger") {
            outVal = CVal::makeBool(std::isfinite(a) && std::trunc(a) == a);
            return true;
        }
    }
    if (recv.globalName == "String" && (m == "fromCharCode" || m == "fromCodePoint")) {
        std::string s;
        for (size_t k = 1; k < opVals.size(); ++k) {
            s.push_back(static_cast<char>(jsToInt32(opV(k).num) & 0x7F));
        }
        outVal = CVal::makeStr(s);
        return true;
    }
    if ((m == "charCodeAt" || m == "codePointAt") && opVals.size() >= 2) {
        int64_t idx = opV(1).i64;
        std::string s = getStr(recv);
        if (idx >= 0 && static_cast<size_t>(idx) < s.size()) {
            outVal = CVal::makeI64(static_cast<unsigned char>(s[static_cast<size_t>(idx)]));
        } else {
            outVal = CVal::makeI64(getProp(recv, std::to_string(idx)).i64 & 0xFFFF);
        }
        return true;
    }
    if (m == "charAt" && opVals.size() >= 2) {
        int64_t idx = opV(1).i64;
        std::string s = getStr(recv);
        outVal = CVal::makeStr((idx >= 0 && static_cast<size_t>(idx) < s.size())
                                   ? std::string(1, s[static_cast<size_t>(idx)]) : "");
        return true;
    }
    if (m == "toLowerCase" || m == "toUpperCase" || m == "toLocaleLowerCase" || m == "toLocaleUpperCase") {
        bool isLow = (m == "toLowerCase" || m == "toLocaleLowerCase");
        std::string s = getStr(recv);
        for (char& ch : s) {
            if (isLow && ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + 32);
            if (!isLow && ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 32);
        }
        outVal = CVal::makeStr(s);
        return true;
    }
    if (m == "substring" && opVals.size() >= 2) {
        std::string s = getStr(recv);
        int64_t len = static_cast<int64_t>(s.size());
        int64_t a = std::clamp<int64_t>(opV(1).i64, 0, len);
        int64_t b = (opVals.size() >= 3 && opV(2).kind != CVal::Kind::Undefined)
                        ? std::clamp<int64_t>(opV(2).i64, 0, len) : len;
        if (a > b) std::swap(a, b);
        outVal = CVal::makeStr(s.substr(static_cast<size_t>(a), static_cast<size_t>(b - a)));
        return true;
    }
    if (m == "substr" && opVals.size() >= 2) {
        std::string s = getStr(recv);
        int64_t len = static_cast<int64_t>(s.size());
        int64_t st = opV(1).i64;
        if (st < 0) st = std::max<int64_t>(0, len + st); else st = std::min(st, len);
        int64_t rem = len - st;
        int64_t cnt = (opVals.size() >= 3 && opV(2).kind != CVal::Kind::Undefined)
                          ? std::clamp<int64_t>(opV(2).i64, 0, rem) : rem;
        outVal = CVal::makeStr(s.substr(static_cast<size_t>(st), static_cast<size_t>(cnt)));
        return true;
    }
    if ((m == "indexOf" || m == "lastIndexOf") && opVals.size() >= 2) {
        bool isLast = (m == "lastIndexOf");
        CVal needle = opV(1);
        if (recv.kind == CVal::Kind::String || needle.kind == CVal::Kind::String) {
            std::string h = getStr(recv), nd = getStr(needle);
            auto pos = isLast ? h.rfind(nd) : h.find(nd);
            outVal = CVal::makeI64(pos == std::string::npos ? -1 : static_cast<int64_t>(pos));
        } else {
            int64_t len = getProp(recv, "length").i64, found = -1;
            if (isLast) {
                for (int64_t i = len - 1; i >= 0; --i) {
                    if (getProp(recv, std::to_string(i)).i64 == needle.i64) { found = i; break; }
                }
            } else {
                for (int64_t i = 0; i < len; ++i) {
                    if (getProp(recv, std::to_string(i)).i64 == needle.i64) { found = i; break; }
                }
            }
            outVal = CVal::makeI64(found);
        }
        return true;
    }
    if (m == "concat" && opVals.size() >= 2) {
        std::string s = getStr(recv);
        for (size_t k = 1; k < opVals.size(); ++k) s += getStr(opV(k));
        outVal = CVal::makeStr(s);
        return true;
    }
    if (m == "push" && opVals.size() >= 2) {
        int64_t len = getProp(recv, "length").i64;
        setProp(recv, std::to_string(len), opV(1));
        setProp(recv, "length", CVal::makeI64(len + 1));
        outVal = CVal::makeI64(len + 1);
        return true;
    }
    if (m == "pop") {
        int64_t len = getProp(recv, "length").i64;
        CVal elem = (len > 0) ? getProp(recv, std::to_string(len - 1)) : CVal::makeI64(0);
        setProp(recv, "length", CVal::makeI64(std::max<int64_t>(0, len - 1)));
        outVal = elem;
        return true;
    }
    if (m == "shift") {
        int64_t len = getProp(recv, "length").i64;
        CVal first = (len > 0) ? getProp(recv, "0") : CVal::makeI64(0);
        for (int64_t i = 0; i + 1 < len; ++i) setProp(recv, std::to_string(i), getProp(recv, std::to_string(i + 1)));
        setProp(recv, "length", CVal::makeI64(std::max<int64_t>(0, len - 1)));
        outVal = first;
        return true;
    }
    if (m == "unshift" && opVals.size() >= 2) {
        int64_t len = getProp(recv, "length").i64;
        for (int64_t i = len; i >= 1; --i) setProp(recv, std::to_string(i), getProp(recv, std::to_string(i - 1)));
        setProp(recv, "0", opV(1));
        setProp(recv, "length", CVal::makeI64(len + 1));
        outVal = CVal::makeI64(len + 1);
        return true;
    }
    if (m == "reverse") {
        int64_t len = std::clamp<int64_t>(getProp(recv, "length").i64, 0, 16);
        std::vector<CVal> tmp(static_cast<size_t>(len));
        for (int64_t i = 0; i < len; ++i) tmp[static_cast<size_t>(i)] = getProp(recv, std::to_string(len - 1 - i));
        for (int64_t i = 0; i < len; ++i) setProp(recv, std::to_string(i), tmp[static_cast<size_t>(i)]);
        outVal = recv;
        return true;
    }
    if (m == "splice" && opVals.size() >= 2) {
        int64_t len = std::clamp<int64_t>(getProp(recv, "length").i64, 0, 16);
        int64_t st = opV(1).i64;
        if (st < 0) st = std::max<int64_t>(0, len + st); else st = std::min(st, len);
        int64_t del = (opVals.size() >= 3 && opV(2).kind != CVal::Kind::Undefined)
                          ? std::clamp<int64_t>(opV(2).i64, 0, len - st) : (len - st);
        uint32_t remOid = allocObj("__local_" + std::to_string(heap.size()));
        CVal remArr = CVal::makeObj(remOid, static_cast<int64_t>(0x00B1000000000000ULL + remOid));
        setProp(remArr, "length", CVal::makeI64(del));
        for (int64_t i = 0; i < del; ++i) setProp(remArr, std::to_string(i), getProp(recv, std::to_string(st + i)));
        std::vector<CVal> nextElems;
        for (int64_t i = 0; i < st; ++i) nextElems.push_back(getProp(recv, std::to_string(i)));
        for (size_t k = 3; k < opVals.size(); ++k) nextElems.push_back(opV(k));
        for (int64_t i = st + del; i < len; ++i) nextElems.push_back(getProp(recv, std::to_string(i)));
        for (size_t i = 0; i < nextElems.size(); ++i) setProp(recv, std::to_string(i), nextElems[i]);
        setProp(recv, "length", CVal::makeI64(static_cast<int64_t>(nextElems.size())));
        outVal = remArr;
        return true;
    }
    if (m == "set" && opVals.size() == 3) {
        CVal src = opV(1);
        int64_t offset = opV(2).i64;
        int64_t sLen = std::clamp<int64_t>(getProp(src, "length").i64, 0, 16);
        for (int64_t i = 0; i < sLen; ++i) {
            if (offset + i >= 0 && offset + i < 16) {
                setProp(recv, std::to_string(offset + i), getProp(src, std::to_string(i)));
            }
        }
        outVal = CVal::makeUndef();
        return true;
    }
    if (m == "join") {
        int64_t len = std::clamp<int64_t>(getProp(recv, "length").i64, 0, 8);
        std::string sep = (opVals.size() >= 2 && opV(1).kind != CVal::Kind::Undefined) ? getStr(opV(1)) : ",";
        std::string s;
        for (int64_t i = 0; i < len; ++i) {
            if (i > 0) s += sep;
            int64_t v = getProp(recv, std::to_string(i)).i64;
            if (v >= 0 && v <= 9) s.push_back(static_cast<char>('0' + v));
            else s.push_back(static_cast<char>(v & 0x7F));
        }
        outVal = CVal::makeStr(s);
        return true;
    }
    if (m == "slice" || m == "subarray") {
        if (recv.kind == CVal::Kind::String) {
            std::string s = getStr(recv);
            int64_t len = static_cast<int64_t>(s.size());
            int64_t st = (opVals.size() >= 2 && opV(1).kind != CVal::Kind::Undefined) ? opV(1).i64 : 0;
            int64_t en = (opVals.size() >= 3 && opV(2).kind != CVal::Kind::Undefined) ? opV(2).i64 : len;
            if (st < 0) st = std::max<int64_t>(0, len + st); else st = std::min(st, len);
            if (en < 0) en = std::max<int64_t>(0, len + en); else en = std::min(en, len);
            outVal = CVal::makeStr(en > st ? s.substr(static_cast<size_t>(st), static_cast<size_t>(en - st)) : "");
            return true;
        }
        int64_t len = getProp(recv, "length").i64;
        int64_t st = (opVals.size() >= 2 && opV(1).kind != CVal::Kind::Undefined) ? opV(1).i64 : 0;
        int64_t en = (opVals.size() >= 3 && opV(2).kind != CVal::Kind::Undefined) ? opV(2).i64 : len;
        if (st < 0) st = std::max<int64_t>(0, len + st); else st = std::min(st, len);
        if (en < 0) en = std::max<int64_t>(0, len + en); else en = std::min(en, len);
        int64_t nl = std::max<int64_t>(0, en - st);
        uint32_t oid = allocObj("__local_" + std::to_string(heap.size()));
        CVal nv = CVal::makeObj(oid, static_cast<int64_t>(0x00B1000000000000ULL + oid));
        setProp(nv, "length", CVal::makeI64(nl));
        for (int64_t i = 0; i < nl; ++i) setProp(nv, std::to_string(i), getProp(recv, std::to_string(st + i)));
        outVal = nv;
        return true;
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
        CVal subRet;
        auto sub = evalFn(*callee, mArgs, recv, 0, depth + 1, false, &subRet);
        if (!sub.completed || sub.threw) {
            res = sub;
            return false;
        }
        outVal = subRet;
        return true;
    }
    if (inModuleInit) {
        outVal = CVal::makeUndef();
        return true;
    }
    res.failReason = "unsupported method: " + m;
    return false;
}

bool ConcreteVM::evalDynamicCall(const il::Instruction& /*inst*/,
                                 const std::vector<CVal>& opVals,
                                 uint32_t depth,
                                 CVal& outVal,
                                 InterpResult& res) {
    auto opV = [&](size_t k) { return (k < opVals.size()) ? opVals[k] : CVal::makeUndef(); };
    CVal calleeV = opV(0);
    std::vector<CVal> cArgs;
    for (size_t k = 2; k < opVals.size(); ++k) cArgs.push_back(opVals[k]);

    if (!calleeV.globalName.empty()) {
        if (calleeV.globalName.rfind("Math.", 0) == 0 &&
            evalMathMethodConcrete(calleeV.globalName.substr(5), cArgs, outVal)) {
            return true;
        }
        if ((calleeV.globalName == "String.fromCharCode" || calleeV.globalName == "String.fromCodePoint")) {
            std::string s;
            for (const auto& a : cArgs) s.push_back(static_cast<char>(jsToInt32(a.num) & 0x7F));
            outVal = CVal::makeStr(s);
            return true;
        }
        if (calleeV.globalName == "isNaN" || calleeV.globalName == "Number.isNaN") {
            outVal = CVal::makeBool(!cArgs.empty() && std::isnan(cArgs[0].num));
            return true;
        }
        if (calleeV.globalName == "isFinite" || calleeV.globalName == "Number.isFinite") {
            outVal = CVal::makeBool(!cArgs.empty() && std::isfinite(cArgs[0].num));
            return true;
        }
        if (calleeV.globalName == "Error" || calleeV.globalName == "RangeError" ||
            calleeV.globalName == "TypeError" || calleeV.globalName == "SyntaxError") {
            outVal = CVal::makeObj(allocObj("__err"));
            return true;
        }
        if (calleeV.globalName == "Number" || calleeV.globalName == "String" ||
            calleeV.globalName == "Boolean" || calleeV.globalName == "Object") {
            outVal = !cArgs.empty() ? cArgs[0] : CVal::makeI64(0);
            return true;
        }
    }
    if (calleeV.kind == CVal::Kind::Function && calleeV.fnIdx < mod.functions.size()) {
        CVal subRet;
        auto sub = evalFn(mod.functions[calleeV.fnIdx], cArgs, opV(1), calleeV.envId, depth + 1, false, &subRet);
        if (!sub.completed || sub.threw) {
            res = sub;
            return false;
        }
        outVal = subRet;
        return true;
    }
    if (calleeV.kind == CVal::Kind::Undefined || calleeV.kind == CVal::Kind::Null) {
        res.completed = true;
        res.threw = true;
        return false;
    }
    if (inModuleInit) {
        outVal = CVal::makeUndef();
        return true;
    }
    res.failReason = "unsupported dynamic call";
    return false;
}

}  // namespace bronze::check
