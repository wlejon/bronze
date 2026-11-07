#include "check/cegis_dag_enum.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace bronze::check {
namespace {

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

uint64_t hashOutputs64(const std::vector<int32_t>& outs) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int32_t v : outs) {
        uint32_t u = static_cast<uint32_t>(v);
        h ^= static_cast<uint64_t>(u & 0xffu);
        h *= 0x100000001b3ULL;
        h ^= static_cast<uint64_t>((u >> 8u) & 0xffu);
        h *= 0x100000001b3ULL;
        h ^= static_cast<uint64_t>((u >> 16u) & 0xffu);
        h *= 0x100000001b3ULL;
        h ^= static_cast<uint64_t>((u >> 24u) & 0xffu);
        h *= 0x100000001b3ULL;
    }
    return h;
}

struct EnumNode {
    std::string expr;
    std::string description;
    uint32_t cost = 1;
    bool hasParam = false;
    std::vector<int32_t> outputs;
};

std::vector<int32_t> buildOrderedConstants(const std::vector<int32_t>& extractedConsts) {
    static const int32_t kPriority[] = {0, 1, -1, 2, 3, 4, 7, 8, 15, 16, 31, 255};
    std::vector<int32_t> ordered;
    std::set<int32_t> seen;
    for (int32_t c : kPriority) {
        if (seen.insert(c).second) ordered.push_back(c);
    }
    for (int32_t c : extractedConsts) {
        if (seen.insert(c).second) ordered.push_back(c);
    }
    return ordered;
}

std::vector<int> buildOrderedShifts(const std::vector<int32_t>& extractedConsts) {
    static const int kPriorityShifts[] = {1, 2, 3, 4, 7, 8, 15, 16, 24, 31};
    std::vector<int> shifts;
    std::set<int> seen;
    for (int s : kPriorityShifts) {
        if (seen.insert(s).second) shifts.push_back(s);
    }
    for (int32_t c : extractedConsts) {
        if (c >= 1 && c <= 31 && seen.insert(static_cast<int>(c)).second) {
            shifts.push_back(static_cast<int>(c));
        }
    }
    for (int s = 1; s <= 31; ++s) {
        if (seen.insert(s).second) shifts.push_back(s);
    }
    return shifts;
}

}  // namespace

DagEnumResult enumerateBottomUpDags(const std::vector<std::string>& paramNames,
                                    const std::vector<int32_t>& extractedConsts,
                                    const std::vector<EnumTestVector>& bank,
                                    uint32_t maxCost,
                                    uint32_t maxEnumeratedNodes,
                                    uint32_t maxMatchingCandidates) {
    DagEnumResult res;
    if (bank.empty() || maxCost == 0 || maxEnumeratedNodes == 0) {
        return res;
    }

    const size_t nVecs = bank.size();
    std::vector<int32_t> expectedVec(nVecs, 0);
    for (size_t j = 0; j < nVecs; ++j) {
        expectedVec[j] = bank[j].expected;
    }
    const uint64_t expectedHash = hashOutputs64(expectedVec);

    std::vector<EnumNode> pool;
    pool.reserve(maxEnumeratedNodes);
    std::unordered_map<uint64_t, std::vector<size_t>> sigTable;
    sigTable.reserve(maxEnumeratedNodes * 2);
    std::set<std::string> seenMatchingExprs;

    uint32_t clampedMaxCost = std::min<uint32_t>(maxCost, 6u);
    std::vector<std::vector<size_t>> tierIndices(clampedMaxCost + 1);

    auto tryRegisterNode = [&](std::string expr, std::string desc, uint32_t cost, bool hasParam,
                               std::vector<int32_t>&& outputs) {
        ++res.totalGenerated;
        uint64_t h = hashOutputs64(outputs);

        if (h == expectedHash && outputs == expectedVec) {
            if (res.matchingCandidates.size() < maxMatchingCandidates &&
                seenMatchingExprs.insert(expr).second) {
                res.matchingCandidates.push_back(
                    {expr, desc, cost, outputs});
            }
        }

        auto it = sigTable.find(h);
        if (it != sigTable.end()) {
            for (size_t idx : it->second) {
                if (pool[idx].outputs == outputs) {
                    ++res.prunedByObsEquiv;
                    return;
                }
            }
        }

        if (pool.size() >= maxEnumeratedNodes) {
            return;
        }

        size_t newIdx = pool.size();
        pool.push_back({std::move(expr), std::move(desc), cost, hasParam, std::move(outputs)});
        sigTable[h].push_back(newIdx);
        if (cost <= clampedMaxCost) {
            tierIndices[cost].push_back(newIdx);
        }
        ++res.uniqueEquivClasses;
    };

    // Tier 1 (cost 1): Parameters and prioritized constants
    for (size_t i = 0; i < paramNames.size(); ++i) {
        std::vector<int32_t> outs(nVecs, 0);
        for (size_t j = 0; j < nVecs; ++j) {
            outs[j] = (i < bank[j].args.size()) ? bank[j].args[i] : 0;
        }
        tryRegisterNode(paramNames[i], "bottom-up enumerated parameter", 1, true, std::move(outs));
    }

    std::vector<int32_t> orderedConsts = buildOrderedConstants(extractedConsts);
    for (int32_t c : orderedConsts) {
        std::vector<int32_t> outs(nVecs, c);
        std::string cs = (c < 0) ? "(" + std::to_string(c) + ")" : std::to_string(c);
        tryRegisterNode(std::move(cs), "bottom-up enumerated constant", 1, false, std::move(outs));
    }

    const std::vector<int> orderedShifts = buildOrderedShifts(extractedConsts);
    const uint32_t maxTotalGenerated = maxEnumeratedNodes * 8u;

    // Tiers 2..maxCost
    for (uint32_t cost = 2; cost <= clampedMaxCost; ++cost) {
        if (pool.size() >= maxEnumeratedNodes || res.totalGenerated >= maxTotalGenerated) break;

        // 1. Binary ops of cost +1 (total cost = c1 + c2): &, |, ^, +, -
        for (uint32_t c1 = 1; c1 < cost; ++c1) {
            uint32_t c2 = cost - c1;
            if (c1 > c2) continue;
            const auto& list1 = tierIndices[c1];
            const auto& list2 = tierIndices[c2];

            for (size_t i1 = 0; i1 < list1.size(); ++i1) {
                if (pool.size() >= maxEnumeratedNodes || res.totalGenerated >= maxTotalGenerated) {
                    break;
                }
                size_t idxA = list1[i1];
                size_t start2 = (c1 == c2) ? i1 : 0;
                for (size_t i2 = start2; i2 < list2.size(); ++i2) {
                    if (pool.size() >= maxEnumeratedNodes ||
                        res.totalGenerated >= maxTotalGenerated) {
                        break;
                    }
                    size_t idxB = list2[i2];
                    const bool hasParam = pool[idxA].hasParam || pool[idxB].hasParam;
                    if (!hasParam && !paramNames.empty()) continue;

                    const std::string exprA = pool[idxA].expr;
                    const std::string exprB = pool[idxB].expr;
                    const std::vector<int32_t> outsA = pool[idxA].outputs;
                    const std::vector<int32_t> outsB = pool[idxB].outputs;

                    if (idxA != idxB) {
                        // (a & b)
                        {
                            std::vector<int32_t> outs(nVecs);
                            for (size_t j = 0; j < nVecs; ++j) outs[j] = outsA[j] & outsB[j];
                            tryRegisterNode("(" + exprA + " & " + exprB + ")",
                                            "bottom-up enumerated bitwise AND", cost, hasParam,
                                            std::move(outs));
                        }
                        // (a | b)
                        {
                            std::vector<int32_t> outs(nVecs);
                            for (size_t j = 0; j < nVecs; ++j) outs[j] = outsA[j] | outsB[j];
                            tryRegisterNode("(" + exprA + " | " + exprB + ")",
                                            "bottom-up enumerated bitwise OR", cost, hasParam,
                                            std::move(outs));
                        }
                        // (a ^ b)
                        {
                            std::vector<int32_t> outs(nVecs);
                            for (size_t j = 0; j < nVecs; ++j) outs[j] = outsA[j] ^ outsB[j];
                            tryRegisterNode("(" + exprA + " ^ " + exprB + ")",
                                            "bottom-up enumerated bitwise XOR", cost, hasParam,
                                            std::move(outs));
                        }
                    }

                    // ((a + b) | 0)
                    {
                        std::vector<int32_t> outs(nVecs);
                        for (size_t j = 0; j < nVecs; ++j) outs[j] = add32(outsA[j], outsB[j]);
                        tryRegisterNode("((" + exprA + " + " + exprB + ") | 0)",
                                        "bottom-up enumerated 32-bit addition", cost, hasParam,
                                        std::move(outs));
                    }

                    // ((a - b) | 0) and ((b - a) | 0)
                    if (idxA != idxB) {
                        {
                            std::vector<int32_t> outs(nVecs);
                            for (size_t j = 0; j < nVecs; ++j) outs[j] = sub32(outsA[j], outsB[j]);
                            tryRegisterNode("((" + exprA + " - " + exprB + ") | 0)",
                                            "bottom-up enumerated 32-bit subtraction", cost,
                                            hasParam, std::move(outs));
                        }
                        {
                            std::vector<int32_t> outs(nVecs);
                            for (size_t j = 0; j < nVecs; ++j) outs[j] = sub32(outsB[j], outsA[j]);
                            tryRegisterNode("((" + exprB + " - " + exprA + ") | 0)",
                                            "bottom-up enumerated 32-bit subtraction", cost,
                                            hasParam, std::move(outs));
                        }
                    }
                }
            }
        }

        // 2. Unary ops of cost +1 (child cost = cost - 1): (-a), (~a)
        const auto& unaryChildren = tierIndices[cost - 1];
        for (size_t idxA : unaryChildren) {
            if (pool.size() >= maxEnumeratedNodes || res.totalGenerated >= maxTotalGenerated) break;
            if (!pool[idxA].hasParam && !paramNames.empty()) continue;

            const std::string exprA = pool[idxA].expr;
            const std::vector<int32_t> outsA = pool[idxA].outputs;
            const bool hasParam = pool[idxA].hasParam;

            {
                std::vector<int32_t> outs(nVecs);
                for (size_t j = 0; j < nVecs; ++j) outs[j] = ~outsA[j];
                tryRegisterNode("(~" + exprA + ")", "bottom-up enumerated bitwise NOT", cost,
                                hasParam, std::move(outs));
            }
            {
                std::vector<int32_t> outs(nVecs);
                for (size_t j = 0; j < nVecs; ++j) outs[j] = sub32(0, outsA[j]);
                tryRegisterNode("((-" + exprA + ") | 0)", "bottom-up enumerated 32-bit negation",
                                cost, hasParam, std::move(outs));
            }
        }

        // 3. Binary multiply of cost +2 (total cost = c1 + c2 + 1): ((a * b) | 0)
        if (cost >= 3) {
            uint32_t sumChildCost = cost - 1;
            for (uint32_t c1 = 1; c1 < sumChildCost; ++c1) {
                uint32_t c2 = sumChildCost - c1;
                if (c1 > c2) continue;
                const auto& list1 = tierIndices[c1];
                const auto& list2 = tierIndices[c2];
                for (size_t i1 = 0; i1 < list1.size(); ++i1) {
                    if (pool.size() >= maxEnumeratedNodes ||
                        res.totalGenerated >= maxTotalGenerated) {
                        break;
                    }
                    size_t idxA = list1[i1];
                    size_t start2 = (c1 == c2) ? i1 : 0;
                    for (size_t i2 = start2; i2 < list2.size(); ++i2) {
                        if (pool.size() >= maxEnumeratedNodes ||
                            res.totalGenerated >= maxTotalGenerated) {
                            break;
                        }
                        size_t idxB = list2[i2];
                        const bool hasParam = pool[idxA].hasParam || pool[idxB].hasParam;
                        if (!hasParam && !paramNames.empty()) continue;

                        const std::string exprA = pool[idxA].expr;
                        const std::string exprB = pool[idxB].expr;
                        const std::vector<int32_t> outsA = pool[idxA].outputs;
                        const std::vector<int32_t> outsB = pool[idxB].outputs;

                        std::vector<int32_t> outs(nVecs);
                        for (size_t j = 0; j < nVecs; ++j) outs[j] = mul32(outsA[j], outsB[j]);
                        tryRegisterNode("((" + exprA + " * " + exprB + ") | 0)",
                                        "bottom-up enumerated 32-bit multiplication", cost,
                                        hasParam, std::move(outs));
                    }
                }
            }
        }

        // 4. Shift ops by constant shift amounts 1..31 (child cost = cost - 1): <<, >>, >>>
        for (int sh : orderedShifts) {
            if (pool.size() >= maxEnumeratedNodes || res.totalGenerated >= maxTotalGenerated) break;
            std::string shStr = std::to_string(sh);
            for (size_t idxA : unaryChildren) {
                if (pool.size() >= maxEnumeratedNodes || res.totalGenerated >= maxTotalGenerated) {
                    break;
                }
                if (!pool[idxA].hasParam && !paramNames.empty()) continue;

                const std::string exprA = pool[idxA].expr;
                const std::vector<int32_t> outsA = pool[idxA].outputs;
                const bool hasParam = pool[idxA].hasParam;

                {
                    std::vector<int32_t> outs(nVecs);
                    for (size_t j = 0; j < nVecs; ++j) outs[j] = shl32(outsA[j], sh);
                    tryRegisterNode("(" + exprA + " << " + shStr + ")",
                                    "bottom-up enumerated left shift", cost, hasParam,
                                    std::move(outs));
                }
                {
                    std::vector<int32_t> outs(nVecs);
                    for (size_t j = 0; j < nVecs; ++j) outs[j] = sar32(outsA[j], sh);
                    tryRegisterNode("(" + exprA + " >> " + shStr + ")",
                                    "bottom-up enumerated arithmetic right shift", cost, hasParam,
                                    std::move(outs));
                }
                {
                    std::vector<int32_t> outs(nVecs);
                    for (size_t j = 0; j < nVecs; ++j) outs[j] = shr32(outsA[j], sh);
                    tryRegisterNode("((" + exprA + " >>> " + shStr + ") | 0)",
                                    "bottom-up enumerated logical right shift", cost, hasParam,
                                    std::move(outs));
                }
            }
        }
    }

    std::stable_sort(res.matchingCandidates.begin(), res.matchingCandidates.end(),
                     [](const SynthesizedDagCandidate& a, const SynthesizedDagCandidate& b) {
                         if (a.cost != b.cost) return a.cost < b.cost;
                         return a.expr.size() < b.expr.size();
                     });
    return res;
}

}  // namespace bronze::check
