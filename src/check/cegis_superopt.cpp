#include "check/cegis_superopt.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <verify/bv_solver.hpp>

#include "check/prove_equiv.h"
#include "check/solve_fn.h"

namespace bronze::check {
namespace {

using brass::verify::BVContext;
using brass::verify::BVNodeId;
using brass::verify::BVOp;
using brass::verify::SolveOutcome;

std::string fnv1aHex64(std::string_view text) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : text) {
        h ^= static_cast<uint64_t>(c);
        h *= 0x100000001b3ULL;
    }
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setw(16) << std::setfill('0') << h;
    return oss.str();
}

const il::Function* findTargetIlFn(const il::Module& mod, std::string_view preferredName) {
    const il::Function* fallback = nullptr;
    for (const auto& fn : mod.functions) {
        if (fn.isExternal || fn.blocks.empty()) continue;
        if (fn.name == "bronze_main" || fn.name == "__main__" || fn.name == "<module>") continue;
        if (!preferredName.empty() && fn.name == preferredName) return &fn;
        if (!fallback) fallback = &fn;
    }
    return fallback;
}

uint32_t estimateIlFunctionCost(const il::Function& fn, std::string_view src) {
    uint32_t cost = 0;
    for (const auto& blk : fn.blocks) {
        for (const auto& inst : blk.instructions) {
            using Op = il::Op;
            switch (inst.op) {
                case Op::Branch:
                    cost += 5;
                    break;
                case Op::Div:
                case Op::Mod:
                    cost += 8;
                    break;
                case Op::Mul:
                case Op::MathImul:
                case Op::Pow:
                    cost += 3;
                    break;
                case Op::Call:
                case Op::MethodCall:
                    cost += 4;
                    break;
                case Op::Add:
                case Op::Sub:
                case Op::Neg:
                case Op::BitAnd:
                case Op::BitOr:
                case Op::BitXor:
                case Op::BitNot:
                case Op::Shl:
                case Op::Shr:
                case Op::UShr:
                case Op::CmpEq:
                case Op::CmpNe:
                case Op::CmpLt:
                case Op::CmpLe:
                case Op::CmpGt:
                case Op::CmpGe:
                case Op::RelLt:
                case Op::RelLe:
                case Op::RelGt:
                case Op::RelGe:
                    cost += 1;
                    break;
                default:
                    break;
            }
        }
    }
    // Count source-level control-flow and calls that IL might simplify
    uint32_t srcBranches = 0;
    for (size_t i = 0; i < src.size(); ++i) {
        if (src.substr(i, 3) == "if " || src.substr(i, 3) == "if(" ||
            src.substr(i, 4) == "for " || src.substr(i, 4) == "for(" ||
            src[i] == '?') {
            srcBranches += 4;
        }
    }
    return std::max<uint32_t>(cost, srcBranches + 2);
}

std::vector<int32_t> extractConstantsFromSource(std::string_view src) {
    std::set<int32_t> found = {0, 1, -1, 2, 3, 4, 7, 8, 15, 16, 31, 255};
    size_t i = 0;
    while (i < src.size()) {
        if (src[i] >= '0' && src[i] <= '9') {
            bool neg = (i > 0 && src[i - 1] == '-');
            size_t start = i;
            if (i + 1 < src.size() && src[i] == '0' && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
                i += 2;
                while (i < src.size() &&
                       ((src[i] >= '0' && src[i] <= '9') ||
                        (src[i] >= 'a' && src[i] <= 'f') ||
                        (src[i] >= 'A' && src[i] <= 'F'))) {
                    ++i;
                }
            } else {
                while (i < src.size() && src[i] >= '0' && src[i] <= '9') ++i;
            }
            std::string tok(src.substr(start, i - start));
            try {
                long long v = std::stoll(tok, nullptr, 0);
                if (neg) v = -v;
                int32_t iv = static_cast<int32_t>(v);
                found.insert(iv);
                if (iv > 1 && iv <= 65536 && (iv & (iv - 1)) == 0) {
                    found.insert(iv - 1);
                    int sh = 0;
                    while ((1 << sh) < iv && sh < 30) ++sh;
                    found.insert(sh);
                }
            } catch (...) {
            }
        } else {
            ++i;
        }
    }
    return std::vector<int32_t>(found.begin(), found.end());
}

struct CandidateDag {
    std::string expr;
    std::string description;
    uint32_t cost = 1;
    std::function<int32_t(const std::vector<int32_t>&)> eval;
};

int32_t sar32(int32_t a, int32_t b) {
    uint32_t sh = static_cast<uint32_t>(b) & 31u;
    return a >> sh;
}

int32_t shr32(int32_t a, int32_t b) {
    uint32_t sh = static_cast<uint32_t>(b) & 31u;
    return static_cast<int32_t>(static_cast<uint32_t>(a) >> sh);
}

int32_t shl32(int32_t a, int32_t b) {
    uint32_t sh = static_cast<uint32_t>(b) & 31u;
    return static_cast<int32_t>(static_cast<uint32_t>(a) << sh);
}

int32_t add32(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

int32_t sub32(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}

int32_t mul32(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}

std::vector<CandidateDag> generateCandidateDags(const std::vector<std::string>& params,
                                                const std::vector<int32_t>& consts,
                                                uint32_t maxCandidates) {
    std::vector<CandidateDag> out;
    out.reserve(maxCandidates);
    std::set<std::string> seen;

    auto addCand = [&](std::string expr, std::string desc, uint32_t cost,
                       std::function<int32_t(const std::vector<int32_t>&)> fn) {
        if (out.size() >= maxCandidates) return;
        if (seen.insert(expr).second) {
            out.push_back({std::move(expr), std::move(desc), cost, std::move(fn)});
        }
    };

    const std::string p0 = params.empty() ? "x" : params[0];

    // 1. Cost-1 Atoms (constants & parameters)
    for (int32_t c : consts) {
        std::string cs = std::to_string(c);
        addCand(cs, "constant folding", 1, [c](const auto&) { return c; });
    }
    for (size_t i = 0; i < params.size(); ++i) {
        addCand(params[i], "identity projection", 1,
                [i](const auto& a) { return i < a.size() ? a[i] : 0; });
    }

    if (params.empty()) return out;

    // 2. Classic Hacker's Delight Branchless Idioms & Strength Reductions on p0
    addCand("((" + p0 + " ^ (" + p0 + " >> 31)) - (" + p0 + " >> 31))",
            "Hacker's Delight branchless abs(x)", 3, [](const auto& a) {
                int32_t x = a[0];
                int32_t m = sar32(x, 31);
                return sub32(x ^ m, m);
            });

    addCand("(" + p0 + " & ~(" + p0 + " >> 31))",
            "branchless max(x, 0) via sign-mask", 2, [](const auto& a) {
                int32_t x = a[0];
                return x & ~sar32(x, 31);
            });

    addCand("(" + p0 + " & (" + p0 + " >> 31))",
            "branchless min(x, 0) via sign-mask", 2, [](const auto& a) {
                int32_t x = a[0];
                return x & sar32(x, 31);
            });

    addCand("((" + p0 + " >> 31) | ((-" + p0 + ") >>> 31))",
            "Hacker's Delight branchless sign(x)", 3, [](const auto& a) {
                int32_t x = a[0];
                uint32_t negX = 0u - static_cast<uint32_t>(x);
                return sar32(x, 31) | static_cast<int32_t>(negX >> 31u);
            });

    addCand("(" + p0 + " & -" + p0 + ")",
            "isolate lowest set bit (x & -x)", 2, [](const auto& a) {
                int32_t x = a[0];
                return x & sub32(0, x);
            });

    addCand("(" + p0 + " & (" + p0 + " - 1))",
            "clear lowest set bit (x & (x - 1))", 2, [](const auto& a) {
                int32_t x = a[0];
                return x & sub32(x, 1);
            });

    addCand("((" + p0 + " * (" + p0 + " - 1)) / 2)",
            "Gauss closed-form series sum 0..(n-1)", 3, [](const auto& a) {
                int64_t n = a[0];
                return static_cast<int32_t>((n * (n - 1)) / 2);
            });

    addCand("((" + p0 + " * (" + p0 + " + 1)) / 2)",
            "Gauss closed-form series sum 0..n", 3, [](const auto& a) {
                int64_t n = a[0];
                return static_cast<int32_t>((n * (n + 1)) / 2);
            });

    addCand("((" + p0 + " & 1) + ((" + p0 + " >> 1) & 1) + ((" + p0 +
                " >> 2) & 1) + ((" + p0 + " >> 3) & 1))",
            "4-bit branchless SWAR popcount", 4, [](const auto& a) {
                int32_t x = a[0];
                return (x & 1) + (sar32(x, 1) & 1) + (sar32(x, 2) & 1) + (sar32(x, 3) & 1);
            });

    // 3. Unary & Constant Binary Operations on p0
    addCand("(-" + p0 + ")", "arithmetic negation", 1,
            [](const auto& a) { return sub32(0, a[0]); });
    addCand("(~" + p0 + ")", "bitwise NOT", 1,
            [](const auto& a) { return ~a[0]; });

    for (int32_t c : consts) {
        if (c == 0) continue;
        std::string cs = std::to_string(c);
        addCand("(" + p0 + " & " + cs + ")", "bitwise AND mask", 2,
                [c](const auto& a) { return a[0] & c; });
        addCand("(" + p0 + " | " + cs + ")", "bitwise OR mask", 2,
                [c](const auto& a) { return a[0] | c; });
        addCand("(" + p0 + " ^ " + cs + ")", "bitwise XOR mask", 2,
                [c](const auto& a) { return a[0] ^ c; });
        addCand("((" + p0 + " + " + cs + ") | 0)", "constant addition", 2,
                [c](const auto& a) { return add32(a[0], c); });
        addCand("((" + p0 + " - " + cs + ") | 0)", "constant subtraction", 2,
                [c](const auto& a) { return sub32(a[0], c); });
        addCand("((" + cs + " - " + p0 + ") | 0)", "reverse constant subtraction", 2,
                [c](const auto& a) { return sub32(c, a[0]); });
        addCand("((" + p0 + " * " + cs + ") | 0)", "constant multiplication", 3,
                [c](const auto& a) { return mul32(a[0], c); });

        if (c >= 1 && c <= 31) {
            addCand("(" + p0 + " << " + cs + ")", "left shift", 2,
                    [c](const auto& a) { return shl32(a[0], c); });
            addCand("(" + p0 + " >> " + cs + ")", "arithmetic right shift", 2,
                    [c](const auto& a) { return sar32(a[0], c); });
            addCand("(" + p0 + " >>> " + cs + ")", "logical right shift", 2,
                    [c](const auto& a) { return shr32(a[0], c); });
            // Sign-extension idiom: (x << s) >> s
            addCand("((" + p0 + " << " + cs + ") >> " + cs + ")",
                    "sign-extension via shift pair", 3,
                    [c](const auto& a) { return sar32(shl32(a[0], c), c); });
        }
    }

    // 4. Depth-2 Shift + Mask Fusions & Clamp Idioms on p0
    for (int sh : {1, 2, 3, 4, 8, 16, 24}) {
        std::string shs = std::to_string(sh);
        for (int32_t m : consts) {
            if (m <= 0) continue;
            std::string ms = std::to_string(m);
            addCand("((" + p0 + " >> " + shs + ") & " + ms + ")",
                    "fused shift-and-mask", 3,
                    [sh, m](const auto& a) { return sar32(a[0], sh) & m; });
            addCand("((" + p0 + " >>> " + shs + ") & " + ms + ")",
                    "fused unsigned-shift-and-mask", 3,
                    [sh, m](const auto& a) { return shr32(a[0], sh) & m; });
            addCand("((" + p0 + " & " + ms + ") << " + shs + ")",
                    "fused mask-and-shift", 3,
                    [sh, m](const auto& a) { return shl32(a[0] & m, sh); });
        }
    }

    // Clamp idioms: clamp(x, lo, hi)
    for (size_t i = 0; i < consts.size(); ++i) {
        for (size_t j = i + 1; j < consts.size(); ++j) {
            int32_t lo = std::min(consts[i], consts[j]);
            int32_t hi = std::max(consts[i], consts[j]);
            if (lo == hi) continue;
            std::string los = std::to_string(lo);
            std::string his = std::to_string(hi);
            addCand("((" + p0 + " < " + los + ") ? " + los + " : ((" + p0 + " > " + his + ") ? " +
                        his + " : " + p0 + "))",
                    "branchless ternary clamp[" + los + ", " + his + "]", 4,
                    [lo, hi](const auto& a) {
                        int32_t x = a[0];
                        return x < lo ? lo : (x > hi ? hi : x);
                    });
        }
    }

    // 5. Two-Parameter Candidate DAGs (p0, p1)
    if (params.size() >= 2) {
        const std::string& p1 = params[1];
        addCand("((" + p0 + " + " + p1 + ") | 0)", "2-input addition", 2,
                [](const auto& a) { return add32(a[0], a[1]); });
        addCand("((" + p0 + " - " + p1 + ") | 0)", "2-input subtraction", 2,
                [](const auto& a) { return sub32(a[0], a[1]); });
        addCand("((" + p1 + " - " + p0 + ") | 0)", "reverse 2-input subtraction", 2,
                [](const auto& a) { return sub32(a[1], a[0]); });
        addCand("((" + p0 + " * " + p1 + ") | 0)", "2-input multiplication", 3,
                [](const auto& a) { return mul32(a[0], a[1]); });
        addCand("(" + p0 + " & " + p1 + ")", "2-input bitwise AND", 2,
                [](const auto& a) { return a[0] & a[1]; });
        addCand("(" + p0 + " | " + p1 + ")", "2-input bitwise OR", 2,
                [](const auto& a) { return a[0] | a[1]; });
        addCand("(" + p0 + " ^ " + p1 + ")", "2-input bitwise XOR", 2,
                [](const auto& a) { return a[0] ^ a[1]; });
        addCand("((" + p0 + " & " + p1 + ") + ((" + p0 + " ^ " + p1 + ") >> 1))",
                "Hacker's Delight overflow-free average", 3,
                [](const auto& a) { return add32(a[0] & a[1], sar32(a[0] ^ a[1], 1)); });
        addCand("((" + p0 + " < " + p1 + ") ? " + p0 + " : " + p1 + ")",
                "min(x, y)", 3,
                [](const auto& a) { return a[0] < a[1] ? a[0] : a[1]; });
        addCand("((" + p0 + " > " + p1 + ") ? " + p0 + " : " + p1 + ")",
                "max(x, y)", 3,
                [](const auto& a) { return a[0] > a[1] ? a[0] : a[1]; });
        addCand("(" + p1 + " ^ ((" + p0 + " ^ " + p1 + ") & ((" + p0 + " - " + p1 + ") >> 31)))",
                "Hacker's Delight branchless min(x, y)", 4,
                [](const auto& a) {
                    return a[1] ^ ((a[0] ^ a[1]) & sar32(sub32(a[0], a[1]), 31));
                });
        addCand("(" + p0 + " ^ ((" + p0 + " ^ " + p1 + ") & ((" + p0 + " - " + p1 + ") >> 31)))",
                "Hacker's Delight branchless max(x, y)", 4,
                [](const auto& a) {
                    return a[0] ^ ((a[0] ^ a[1]) & sar32(sub32(a[0], a[1]), 31));
                });

        for (int c0 = -4; c0 <= 4; ++c0) {
            for (int c1 = -4; c1 <= 4; ++c1) {
                if (c0 == 0 && c1 == 0) continue;
                std::string expr = "((" + std::to_string(c0) + " * " + p0 + " + " +
                                   std::to_string(c1) + " * " + p1 + ") | 0)";
                addCand(expr, "linear combination", 4, [c0, c1](const auto& a) {
                    return add32(mul32(c0, a[0]), mul32(c1, a[1]));
                });
            }
        }
    }

    std::stable_sort(out.begin(), out.end(), [](const CandidateDag& a, const CandidateDag& b) {
        if (a.cost != b.cost) return a.cost < b.cost;
        return a.expr.size() < b.expr.size();
    });
    return out;
}

std::string buildCandidateFunctionSource(std::string_view fnName,
                                         const std::vector<std::string>& params,
                                         std::string_view expr) {
    std::ostringstream oss;
    oss << "function " << (fnName.empty() ? "__opt" : fnName) << "(";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << params[i];
    }
    oss << ") {\n  return " << expr << ";\n}";
    return oss.str();
}

}  // namespace

CegisSuperoptResult superoptimizeJsFunction(std::string_view fnSource,
                                            const CegisSuperoptOptions& opts) {
    auto t0 = std::chrono::steady_clock::now();
    CegisSuperoptResult res;
    res.originalSource = std::string(fnSource);

    std::string refFnName;
    std::string normalized = normalizeJsFunctionSource(fnSource, &refFnName, "__superopt_fn");
    il::Module refMod;
    std::string lowerErr;
    if (!lowerSingleSourceForEquiv(normalized, "<cegis_superopt_ref.js>", refMod, &lowerErr)) {
        res.error = "failed to lower reference function: " + lowerErr;
        res.summary = res.error;
        return res;
    }

    const il::Function* refFn = findTargetIlFn(refMod, refFnName);
    if (!refFn) {
        res.error = "no function found to superoptimize";
        res.summary = res.error;
        return res;
    }

    res.functionName = refFn->name;
    uint32_t numParams = static_cast<uint32_t>(refFn->callerParamCount());
    for (uint32_t i = 0; i < numParams; ++i) {
        std::string pname;
        if ((refFn->firstSourceParam() + i) < refFn->params.size()) {
            pname = refFn->params[refFn->firstSourceParam() + i].name;
        }
        if (pname.empty()) pname = "arg_" + std::to_string(i);
        res.paramNames.push_back(pname);
    }

    res.stats.costBefore = estimateIlFunctionCost(*refFn, fnSource);
    res.stats.costAfter = res.stats.costBefore;

    // Build symbolic representation of the reference function once so we can
    // evaluate concrete inputs in nanoseconds AND run SMT counterexample queries!
    BVContext refCtx;
    std::vector<BVNodeId> refArgNodes;
    std::vector<std::string> refVarNames;
    for (uint32_t i = 0; i < numParams; ++i) {
        std::string vname = "arg_" + std::to_string(i);
        refVarNames.push_back(vname);
        BVNodeId raw32 = refCtx.make_var(vname, 32);
        refArgNodes.push_back(refCtx.make_sext(raw32, 32));
    }

    BVNodeId refRetNode = 0;
    std::string symReason;
    if (!evalIlFunctionSymbolic(refCtx, refMod, *refFn, refArgNodes, true, &refRetNode, &symReason)) {
        res.error = "reference function not symbolically decidable: " + symReason;
        res.summary = res.error;
        return res;
    }

    auto evalRefConcrete = [&](const std::vector<int32_t>& args) -> int32_t {
        std::vector<uint64_t> env(numParams, 0);
        for (uint32_t i = 0; i < numParams; ++i) {
            int32_t v = (i < args.size()) ? args[i] : 0;
            env[i] = static_cast<uint32_t>(v);
        }
        return static_cast<int32_t>(static_cast<uint32_t>(refCtx.eval_concrete(refRetNode, env)));
    };

    // Seed concrete test-vector bank T
    std::vector<int32_t> seedScalars = {
        0, 1, -1, 2, -2, 3, -3, 4, -4, 7, -7, 8, -8, 15, 16,
        31, 32, 42, -42, 63, 64, 100, -100, 127, -128, 255, 256,
        1024, -1024, 0x55555555, 0x7fffffff, static_cast<int32_t>(0x80000000u)};

    struct TestVector {
        std::vector<int32_t> args;
        int32_t expected = 0;
    };
    std::vector<TestVector> bank;

    auto clampDom = [&](int32_t v) -> int32_t {
        if (v < opts.domainMin) return opts.domainMin;
        if (v > opts.domainMax) return opts.domainMax;
        return v;
    };

    if (numParams == 0) {
        bank.push_back({{}, evalRefConcrete({})});
    } else if (numParams == 1) {
        std::set<int32_t> uniq;
        for (int32_t s : seedScalars) uniq.insert(clampDom(s));
        for (int32_t v : uniq) {
            bank.push_back({{v}, evalRefConcrete({v})});
        }
    } else {
        // Pairwise boundary combinations for 2+ params
        std::vector<int32_t> shortSeeds = {0, 1, -1, 2, -2, 7, -8, 15, 31, 42, -42, 127, -128, 255};
        for (int32_t a : shortSeeds) {
            for (int32_t b : shortSeeds) {
                std::vector<int32_t> vec(numParams, 0);
                vec[0] = clampDom(a);
                vec[1] = clampDom(b);
                if (numParams >= 3) vec[2] = clampDom(a ^ b);
                bank.push_back({vec, evalRefConcrete(vec)});
                if (bank.size() >= 64) break;
            }
            if (bank.size() >= 64) break;
        }
    }
    res.stats.seedVectorCount = static_cast<uint32_t>(bank.size());

    std::vector<int32_t> extractedConsts = extractConstantsFromSource(fnSource);
    std::vector<CandidateDag> candidates =
        generateCandidateDags(res.paramNames, extractedConsts, opts.maxCandidates);
    res.stats.candidatesGenerated = static_cast<uint32_t>(candidates.size());

    for (const auto& cand : candidates) {
        if (opts.requireLowerCost && cand.cost > res.stats.costBefore) {
            continue;
        }
        ++res.stats.candidatesTested;

        // Step 1: Nanosecond concrete pruning against test-vector bank T
        bool matchedAllConcrete = true;
        for (const auto& tv : bank) {
            if (cand.eval(tv.args) != tv.expected) {
                matchedAllConcrete = false;
                break;
            }
        }
        if (!matchedAllConcrete) {
            ++res.stats.concretePruned;
            continue;
        }

        // Step 2: Candidate survived all concrete test vectors in T!
        // Query the bitvector SMT solver for a counterexample across the full domain.
        if (res.stats.smtQueries >= opts.maxSmtQueries) {
            break;
        }
        ++res.stats.smtQueries;

        std::string candFnSrc =
            buildCandidateFunctionSource(res.functionName, res.paramNames, cand.expr);
        std::string eqErr;
        auto eqReport =
            proveSourceEquivalent(normalized, candFnSrc, "", opts.domainMin, opts.domainMax, &eqErr);

        if (eqReport.isEquivalent()) {
            res.verified = true;
            res.optimized = (cand.cost < res.stats.costBefore) ||
                            (candFnSrc.size() + 8 < fnSource.size());
            res.synthesizedExpression = cand.expr;
            res.optimizedSource = candFnSrc;
            res.ruleDescription = cand.description;
            res.stats.costAfter = cand.cost;
            res.stats.finalVectorCount = static_cast<uint32_t>(bank.size());
            res.certificateHash = fnv1aHex64(normalized + "===" + candFnSrc);
            auto t1 = std::chrono::steady_clock::now();
            res.stats.elapsedMs =
                std::chrono::duration<double, std::milli>(t1 - t0).count();
            std::ostringstream sum;
            sum << "CEGIS superoptimizer synthesized `" << cand.expr << "` (" << cand.description
                << "): tested " << res.stats.candidatesTested << " candidates, pruned "
                << res.stats.concretePruned << " concretely, " << res.stats.smtQueries
                << " SMT queries, " << res.stats.counterexamplesFound
                << " counterexamples; cost " << res.stats.costBefore << " -> "
                << res.stats.costAfter;
            res.summary = sum.str();
            return res;
        }

        // Extract the newly discovered counterexample and add it to T!
        if (eqReport.hasCounterexample()) {
            std::vector<int32_t> cexVec;
            for (int64_t v : eqReport.counterexampleArgs) {
                cexVec.push_back(static_cast<int32_t>(v));
            }
            if (!cexVec.empty()) {
                int32_t expected = static_cast<int32_t>(eqReport.expectedOutput);
                bank.push_back({cexVec, expected});
                ++res.stats.counterexamplesFound;

                CounterexampleInfo cex;
                cex.round = res.stats.smtQueries;
                cex.candidateSource = cand.expr;
                cex.args = eqReport.counterexampleArgs;
                cex.expectedOutput = eqReport.expectedOutput;
                cex.actualOutput = eqReport.actualOutput;
                res.counterexamples.push_back(std::move(cex));
            }
        }
    }

    res.stats.finalVectorCount = static_cast<uint32_t>(bank.size());
    auto t1 = std::chrono::steady_clock::now();
    res.stats.elapsedMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    res.optimizedSource = std::string(fnSource);
    res.summary = "No lower-cost equivalent expression found after testing " +
                  std::to_string(res.stats.candidatesTested) + " candidates (" +
                  std::to_string(res.stats.concretePruned) + " concretely pruned)";
    return res;
}

}  // namespace bronze::check
