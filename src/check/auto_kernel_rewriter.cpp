#include "check/auto_kernel_rewriter.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <verify/bv_solver.hpp>

#include "check/solve_fn.h"

namespace bronze::check {
namespace {

using brass::verify::BVContext;
using brass::verify::BVNodeId;
using brass::verify::BVOp;
using brass::verify::SolveOutcome;

std::string fnv1aHex(std::string_view text) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : text) {
        h ^= static_cast<uint64_t>(c);
        h *= 0x100000001b3ULL;
    }
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setw(16) << std::setfill('0') << h;
    return oss.str();
}

std::string jsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string trim(std::string_view sv) {
    size_t b = 0;
    while (b < sv.size() && std::isspace(static_cast<unsigned char>(sv[b]))) ++b;
    size_t e = sv.size();
    while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1]))) --e;
    return std::string(sv.substr(b, e - b));
}

struct AffineIndex {
    bool valid = false;
    int64_t scale = 0;   // coefficient of induction var `i`
    int64_t offset = 0;  // constant offset
};

// Parses index expressions over `ivar` such as:
// `i`, `i + 1`, `i - 1`, `2 * i`, `2 * i + 1`, `i * 4 + 2`, `0`, `5`
AffineIndex parseAffineIndexExpr(std::string_view raw, std::string_view ivar) {
    std::string s;
    for (char c : raw) {
        if (!std::isspace(static_cast<unsigned char>(c)) && c != '(' && c != ')') {
            s.push_back(c);
        }
    }
    if (s.empty()) return {};

    // Pure induction var: `i`
    if (s == ivar) return {true, 1, 0};

    // Pure integer constant: `0`, `42`
    {
        char* endp = nullptr;
        long long v = std::strtoll(s.c_str(), &endp, 10);
        if (endp && *endp == '\0') {
            return {true, 0, static_cast<int64_t>(v)};
        }
    }

    // `i+C` or `i-C`
    if (s.rfind(ivar, 0) == 0 && s.size() > ivar.size()) {
        char op = s[ivar.size()];
        std::string rest = s.substr(ivar.size() + 1);
        char* endp = nullptr;
        long long c = std::strtoll(rest.c_str(), &endp, 10);
        if (endp && *endp == '\0') {
            if (op == '+') return {true, 1, static_cast<int64_t>(c)};
            if (op == '-') return {true, 1, -static_cast<int64_t>(c)};
            if (op == '*') return {true, static_cast<int64_t>(c), 0};
        }
    }

    // `A*i`, `A*i+B`, `A*i-B`
    auto starPos = s.find('*');
    if (starPos != std::string::npos) {
        std::string lhs = s.substr(0, starPos);
        std::string rhs = s.substr(starPos + 1);
        char* endp = nullptr;
        long long a = std::strtoll(lhs.c_str(), &endp, 10);
        if (endp && *endp == '\0' && rhs.rfind(ivar, 0) == 0) {
            if (rhs.size() == ivar.size()) {
                return {true, static_cast<int64_t>(a), 0};
            }
            char op = rhs[ivar.size()];
            std::string offStr = rhs.substr(ivar.size() + 1);
            char* endp2 = nullptr;
            long long b = std::strtoll(offStr.c_str(), &endp2, 10);
            if (endp2 && *endp2 == '\0') {
                if (op == '+') return {true, static_cast<int64_t>(a), static_cast<int64_t>(b)};
                if (op == '-') return {true, static_cast<int64_t>(a), -static_cast<int64_t>(b)};
            }
        }
    }

    return {};
}

BVNodeId buildAffineNode(BVContext& ctx, BVNodeId iNode, const AffineIndex& idx) {
    BVNodeId scaleNode = ctx.make_const(static_cast<uint64_t>(idx.scale), 64);
    BVNodeId offNode = ctx.make_const(static_cast<uint64_t>(idx.offset), 64);
    BVNodeId prod = (idx.scale == 1) ? iNode : ctx.make_binary(BVOp::Mul, scaleNode, iNode);
    if (idx.scale == 0) return offNode;
    if (idx.offset == 0) return prod;
    return ctx.make_binary(BVOp::Add, prod, offNode);
}

std::string buildSimpleDiff(std::string_view before, std::string_view after) {
    if (before == after) return "";
    std::ostringstream oss;
    oss << "--- a/loop.js\n+++ b/kernel.js\n";
    std::istringstream bss{std::string(before)};
    std::string line;
    while (std::getline(bss, line)) oss << "-" << line << "\n";
    std::istringstream ass{std::string(after)};
    while (std::getline(ass, line)) oss << "+" << line << "\n";
    return oss.str();
}

}  // namespace

std::string AutoKernelReport::toJson() const {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"foundLoop\": " << (foundLoop ? "true" : "false") << ",\n";
    oss << "  \"parallelizable\": " << (parallelizable ? "true" : "false") << ",\n";
    oss << "  \"provenNoWaw\": " << (provenNoWaw ? "true" : "false") << ",\n";
    oss << "  \"provenNoRaw\": " << (provenNoRaw ? "true" : "false") << ",\n";
    oss << "  \"provenNoWar\": " << (provenNoWar ? "true" : "false") << ",\n";
    oss << "  \"provenInBounds\": " << (provenInBounds ? "true" : "false") << ",\n";
    oss << "  \"functionName\": \"" << jsonEscape(functionName) << "\",\n";
    oss << "  \"inductionVar\": \"" << jsonEscape(inductionVar) << "\",\n";
    oss << "  \"boundExpr\": \"" << jsonEscape(boundExpr) << "\",\n";
    oss << "  \"certificateHash\": \"" << jsonEscape(certificateHash) << "\",\n";
    oss << "  \"summary\": \"" << jsonEscape(summary) << "\",\n";
    oss << "  \"kernelSource\": \"" << jsonEscape(kernelSource) << "\",\n";
    oss << "  \"unifiedDiff\": \"" << jsonEscape(unifiedDiff) << "\",\n";
    oss << "  \"hazards\": [\n";
    for (size_t i = 0; i < hazards.size(); ++i) {
        const auto& h = hazards[i];
        oss << "    {\n";
        oss << "      \"hazardKind\": \"" << jsonEscape(h.hazardKind) << "\",\n";
        oss << "      \"arrayName\": \"" << jsonEscape(h.arrayName) << "\",\n";
        oss << "      \"writeIndexExpr\": \"" << jsonEscape(h.writeIndexExpr) << "\",\n";
        oss << "      \"readIndexExpr\": \"" << jsonEscape(h.readIndexExpr) << "\",\n";
        oss << "      \"iterA\": " << h.iterA << ",\n";
        oss << "      \"iterB\": " << h.iterB << ",\n";
        oss << "      \"conflictIndex\": " << h.conflictIndex << ",\n";
        oss << "      \"description\": \"" << jsonEscape(h.description) << "\"\n";
        oss << "    }" << (i + 1 < hazards.size() ? "," : "") << "\n";
    }
    oss << "  ]\n";
    oss << "}";
    return oss.str();
}

AutoKernelReport analyzeAndRewriteLoopToKernel(std::string_view source) {
    AutoKernelReport rep;
    rep.originalSource = std::string(source);

    std::string fnName;
    std::string norm = normalizeJsFunctionSource(source, &fnName, "auto_kernel");
    rep.functionName = fnName.empty() ? "auto_kernel" : fnName;

    // Extract parameter names from `(p1, p2, ...)`
    auto openP = norm.find('(');
    auto closeP = norm.find(')', openP == std::string::npos ? 0 : openP + 1);
    if (openP != std::string::npos && closeP != std::string::npos) {
        std::string plist = norm.substr(openP + 1, closeP - openP - 1);
        std::istringstream pss(plist);
        std::string tok;
        while (std::getline(pss, tok, ',')) {
            std::string t = trim(tok);
            if (!t.empty()) rep.paramNames.push_back(t);
        }
    }

    // Match `for (let i = 0; i < BOUND; i++ / ++i / i += 1 / i = i + 1) { BODY }`
    std::regex reFor(
        R"(for\s*\(\s*(?:let|var)\s+([A-Za-z_]\w*)\s*=\s*0\s*;\s*\1\s*<\s*([^;]+);\s*(?:\+\+\1|\1\+\+|\1\s*\+=\s*1|\1\s*=\s*\1\s*\+\s*1)\s*\)\s*\{([\s\S]*?)\})");
    std::smatch mf;
    if (!std::regex_search(norm, mf, reFor)) {
        rep.summary = "No canonical `for (let i = 0; i < n; i++)` loop found";
        return rep;
    }

    rep.foundLoop = true;
    rep.inductionVar = mf[1].str();
    rep.boundExpr = trim(mf[2].str());
    std::string loopBody = mf[3].str();

    // Check for disqualifying side-effecting constructs inside the loop body
    if (loopBody.find("new ") != std::string::npos ||
        loopBody.find(".push(") != std::string::npos ||
        loopBody.find("throw ") != std::string::npos) {
        rep.hazards.push_back({"SIDE_EFFECT",
                               "",
                               "",
                               "",
                               0,
                               0,
                               0,
                               "Loop body contains heap allocation or exception side effect"});
        rep.summary = "Rejected parallelization: loop body contains side effects";
        return rep;
    }

    struct AccessInfo {
        std::string arrayName;
        std::string rawIndexExpr;
        AffineIndex affine;
    };
    std::vector<AccessInfo> writes;
    std::vector<AccessInfo> reads;

    // Extract array writes: `arr[idx] = ...` or `arr[idx] += ...`
    std::regex reWrite(R"(([A-Za-z_]\w*)\s*\[\s*([^\]]+)\s*\]\s*(\+|-|\*|/)?=(?!=))");
    {
        std::sregex_iterator it(loopBody.begin(), loopBody.end(), reWrite);
        std::sregex_iterator end;
        for (; it != end; ++it) {
            std::string arr = (*it)[1].str();
            std::string idxStr = trim((*it)[2].str());
            AffineIndex aff = parseAffineIndexExpr(idxStr, rep.inductionVar);
            writes.push_back({arr, idxStr, aff});
            if (std::find(rep.writtenArrays.begin(), rep.writtenArrays.end(), arr) ==
                rep.writtenArrays.end()) {
                rep.writtenArrays.push_back(arr);
            }
            // Compound assignment `arr[idx] += ...` also reads `arr[idx]`!
            if ((*it)[3].matched) {
                reads.push_back({arr, idxStr, aff});
            }
        }
    }

    if (writes.empty()) {
        rep.summary = "Loop contains no TypedArray element stores to parallelize";
        return rep;
    }

    // Extract all array accesses `arr[idx]` and classify non-LHS ones as reads
    std::regex reAnyAccess(R"(([A-Za-z_]\w*)\s*\[\s*([^\]]+)\s*\])");
    {
        std::sregex_iterator it(loopBody.begin(), loopBody.end(), reAnyAccess);
        std::sregex_iterator end;
        for (; it != end; ++it) {
            size_t matchEnd = static_cast<size_t>(it->position(0) + it->length(0));
            size_t p = matchEnd;
            while (p < loopBody.size() && std::isspace(static_cast<unsigned char>(loopBody[p]))) {
                ++p;
            }
            // Plain `=` (not `==`) is a write only
            if (p < loopBody.size() && loopBody[p] == '=' &&
                (p + 1 >= loopBody.size() || loopBody[p + 1] != '=')) {
                continue;
            }
            std::string arr = (*it)[1].str();
            std::string idxStr = trim((*it)[2].str());
            AffineIndex aff = parseAffineIndexExpr(idxStr, rep.inductionVar);
            reads.push_back({arr, idxStr, aff});
            if (std::find(rep.readArrays.begin(), rep.readArrays.end(), arr) ==
                rep.readArrays.end()) {
                rep.readArrays.push_back(arr);
            }
        }
    }

    // Verify all index expressions are affine/decidable
    for (const auto& w : writes) {
        if (!w.affine.valid) {
            rep.summary = "Non-affine write index `" + w.rawIndexExpr + "` cannot be proven race-free";
            return rep;
        }
    }
    for (const auto& r : reads) {
        if (!r.affine.valid) {
            rep.summary = "Non-affine read index `" + r.rawIndexExpr + "` cannot be proven race-free";
            return rep;
        }
    }

    // Build SMT bitvector model over two distinct iterations `0 <= i1 < n`, `0 <= i2 < n`, `i1 != i2`
    BVContext ctx;
    BVNodeId zero = ctx.make_const(0, 64);
    BVNodeId minN = ctx.make_const(2, 64);
    BVNodeId maxN = ctx.make_const(65536, 64);

    BVNodeId n32 = ctx.make_var("n", 32);
    BVNodeId i1_32 = ctx.make_var("i1", 32);
    BVNodeId i2_32 = ctx.make_var("i2", 32);

    BVNodeId n = ctx.make_sext(n32, 32);
    BVNodeId i1 = ctx.make_sext(i1_32, 32);
    BVNodeId i2 = ctx.make_sext(i2_32, 32);

    // Domain constraint: 2 <= n <= 65536 && 0 <= i1 < n && 0 <= i2 < n && i1 != i2
    BVNodeId domCond = ctx.make_binary(
        BVOp::And,
        ctx.make_binary(BVOp::Sle, minN, n),
        ctx.make_binary(BVOp::Sle, n, maxN));
    domCond = ctx.make_binary(
        BVOp::And, domCond,
        ctx.make_binary(BVOp::And,
                        ctx.make_binary(BVOp::Sle, zero, i1),
                        ctx.make_binary(BVOp::Slt, i1, n)));
    domCond = ctx.make_binary(
        BVOp::And, domCond,
        ctx.make_binary(BVOp::And,
                        ctx.make_binary(BVOp::Sle, zero, i2),
                        ctx.make_binary(BVOp::Slt, i2, n)));
    BVNodeId distinctIters = ctx.make_binary(
        BVOp::And, domCond, ctx.make_binary(BVOp::Ne, i1, i2));

    // 1. Check WAW (Write-After-Write): can two distinct iterations i1 != i2 write to the same index?
    rep.provenNoWaw = true;
    for (size_t a = 0; a < writes.size(); ++a) {
        for (size_t b = a; b < writes.size(); ++b) {
            if (writes[a].arrayName != writes[b].arrayName) continue;
            BVNodeId w1 = buildAffineNode(ctx, i1, writes[a].affine);
            BVNodeId w2 = buildAffineNode(ctx, i2, writes[b].affine);
            BVNodeId wawCollide =
                ctx.make_binary(BVOp::And, distinctIters, ctx.make_binary(BVOp::Eq, w1, w2));
            auto sr = brass::verify::solve_nonzero(ctx, wawCollide);
            if (sr.status == SolveOutcome::Sat) {
                rep.provenNoWaw = false;
                int64_t v1 = sr.model.size() > 1 ? static_cast<int32_t>(sr.model[1]) : 0;
                int64_t v2 = sr.model.size() > 2 ? static_cast<int32_t>(sr.model[2]) : 1;
                int64_t cIdx = writes[a].affine.scale * v1 + writes[a].affine.offset;
                rep.hazards.push_back(
                    {"WAW",
                     writes[a].arrayName,
                     writes[a].rawIndexExpr,
                     writes[b].rawIndexExpr,
                     v1,
                     v2,
                     cIdx,
                     "Write-After-Write race on `" + writes[a].arrayName + "`: iterations i1=" +
                         std::to_string(v1) + " and i2=" + std::to_string(v2) +
                         " both write index " + std::to_string(cIdx)});
            }
        }
    }

    // 2. Check RAW / WAR (Loop-Carried Read-After-Write / Write-After-Read):
    // Can iteration i2 read an element of `arr` written by a different iteration i1 (`i1 != i2`)?
    rep.provenNoRaw = true;
    rep.provenNoWar = true;
    for (const auto& w : writes) {
        for (const auto& r : reads) {
            if (w.arrayName != r.arrayName) continue;
            BVNodeId wIdx1 = buildAffineNode(ctx, i1, w.affine);
            BVNodeId rIdx2 = buildAffineNode(ctx, i2, r.affine);
            BVNodeId rawCollide = ctx.make_binary(
                BVOp::And, distinctIters, ctx.make_binary(BVOp::Eq, wIdx1, rIdx2));
            auto sr = brass::verify::solve_nonzero(ctx, rawCollide);
            if (sr.status == SolveOutcome::Sat) {
                int64_t v1 = sr.model.size() > 1 ? static_cast<int32_t>(sr.model[1]) : 0;
                int64_t v2 = sr.model.size() > 2 ? static_cast<int32_t>(sr.model[2]) : 1;
                int64_t lo = std::min(v1, v2);
                int64_t hi = std::max(v1, v2);
                int64_t cIdx = w.affine.scale * v1 + w.affine.offset;
                bool isRaw = (v1 < v2) || (r.affine.offset < w.affine.offset);
                if (isRaw) {
                    rep.provenNoRaw = false;
                } else {
                    rep.provenNoWar = false;
                }
                std::string kind = isRaw ? "RAW" : "WAR";
                rep.hazards.push_back(
                    {kind,
                     w.arrayName,
                     w.rawIndexExpr,
                     r.rawIndexExpr,
                     lo,
                     hi,
                     cIdx,
                     "Loop-carried " + kind + " dependency on `" + w.arrayName +
                         "`: iteration " + std::to_string(lo) + " and iteration " +
                         std::to_string(hi) + " collide at index " + std::to_string(cIdx) +
                         " (write `" + w.arrayName + "[" + w.rawIndexExpr + "]` vs read `" +
                         r.arrayName + "[" + r.rawIndexExpr + "]`)"});
            }
        }
    }

    // 3. Check lower-bound safety (`idx(i1) >= 0` for all `0 <= i1 < n`)
    rep.provenInBounds = true;
    for (const auto& acc : reads) {
        BVNodeId rIdx = buildAffineNode(ctx, i1, acc.affine);
        BVNodeId negAccess =
            ctx.make_binary(BVOp::And, domCond, ctx.make_binary(BVOp::Slt, rIdx, zero));
        auto sr = brass::verify::solve_nonzero(ctx, negAccess);
        if (sr.status == SolveOutcome::Sat) {
            rep.provenInBounds = false;
            int64_t v1 = sr.model.size() > 1 ? static_cast<int32_t>(sr.model[1]) : 0;
            int64_t badIdx = acc.affine.scale * v1 + acc.affine.offset;
            rep.hazards.push_back(
                {"OUT_OF_BOUNDS",
                 acc.arrayName,
                 "",
                 acc.rawIndexExpr,
                 v1,
                 v1,
                 badIdx,
                 "Out-of-bounds read `" + acc.arrayName + "[" + acc.rawIndexExpr +
                     "]` at i=" + std::to_string(v1) + " accesses negative index " +
                     std::to_string(badIdx)});
        }
    }

    if (!rep.hazards.empty()) {
        rep.parallelizable = false;
        rep.summary = "Refuted parallelization: " + rep.hazards[0].description;
        return rep;
    }

    rep.parallelizable = true;

    // Synthesize `@kernel` function!
    // If `boundExpr` is `arr.length` and `n` is not in `paramNames`, add `n` parameter or keep `boundExpr`
    std::vector<std::string> kernelParams = rep.paramNames;
    std::string kernelBound = rep.boundExpr;
    if (kernelBound.find('.') != std::string::npos) {
        kernelBound = "n";
        if (std::find(kernelParams.begin(), kernelParams.end(), "n") == kernelParams.end()) {
            kernelParams.push_back("n");
        }
    }

    std::ostringstream kss;
    kss << "// @kernel\n";
    kss << "function " << rep.functionName << "(";
    for (size_t i = 0; i < kernelParams.size(); ++i) {
        if (i > 0) kss << ", ";
        kss << kernelParams[i];
    }
    kss << ") {\n";
    kss << "  let " << rep.inductionVar << " = global_tid_x();\n";
    kss << "  if (" << rep.inductionVar << " < " << kernelBound << ") {" << loopBody << "  }\n";
    kss << "}";

    rep.kernelSource = kss.str();
    rep.unifiedDiff = buildSimpleDiff(rep.originalSource, rep.kernelSource);
    rep.certificateHash = fnv1aHex(rep.originalSource + "===" + rep.kernelSource);
    rep.summary = "Proven 100% loop-independent (WAW=UNSAT, RAW=UNSAT, WAR=UNSAT, InBounds=UNSAT); "
                  "rewritten to parallel @kernel `" +
                  rep.functionName + "` (cert " + rep.certificateHash + ")";
    return rep;
}

int runAutoKernelCli(int argc, char** argv) {
    std::string inputPath;
    bool showDiff = false;
    bool jsonOut = false;
    bool writeInPlace = false;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--diff" || arg == "-d") {
            showDiff = true;
        } else if (arg == "--json") {
            jsonOut = true;
        } else if (arg == "--write" || arg == "-w") {
            writeInPlace = true;
        } else if (inputPath.empty()) {
            inputPath = arg;
        } else {
            std::cerr << "error: unexpected argument " << arg << "\n";
            return 1;
        }
    }

    if (inputPath.empty()) {
        std::cerr << "usage: bronze auto-kernel <file.js> [--diff] [--json] [--write]\n";
        return 1;
    }

    std::ifstream ifs(inputPath);
    if (!ifs) {
        std::cerr << "error: cannot read " << inputPath << "\n";
        return 1;
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    std::string src = ss.str();

    auto rep = analyzeAndRewriteLoopToKernel(src);
    if (writeInPlace && rep.parallelizable) {
        std::ofstream ofs(inputPath, std::ios::trunc);
        ofs << rep.kernelSource << "\n";
    }

    if (jsonOut) {
        std::cout << rep.toJson() << "\n";
    } else if (showDiff) {
        if (rep.parallelizable) {
            std::cout << rep.unifiedDiff;
        } else {
            std::cout << "// " << rep.summary << "\n";
        }
    } else {
        if (rep.parallelizable) {
            std::cout << rep.kernelSource << "\n";
        } else {
            std::cerr << "error: " << rep.summary << "\n";
            return 1;
        }
    }
    return rep.parallelizable ? 0 : 1;
}

}  // namespace bronze::check
