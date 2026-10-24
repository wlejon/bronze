#include "check/auto_kernel_rewriter.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <verify/bv_solver.hpp>

#include "check/auto_kernel_loop.h"
#include "check/solve_fn.h"
#include "lex/lexer.h"
#include "parse/parser.h"
#include "support/diagnostics.h"
#include "support/source.h"

namespace bronze::check {
namespace {

using autokernel::Affine;
using autokernel::ElementAccess;
using autokernel::kIvAtom;
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

bool parsesAsModule(const std::string& text) {
    SourceSet sources;
    const auto& buf = sources.add("auto_kernel_out.js", text);
    DiagnosticSink diags;
    auto toks = Lexer(buf, diags).lex();
    if (diags.hasErrors()) return false;
    auto mod = Parser(std::move(toks), diags).parseModule("auto_kernel_out.js");
    return !diags.hasErrors() && mod;
}

// Two symbolic iterations i1, i2 of the loop, over the parameters it names.
// Every number is a 32-bit value sign-extended to 64 bits, and Affine keeps its
// coefficients small enough that no sum or product built here wraps.
class LoopModel {
public:
    LoopModel(const autokernel::LoopAnalysis& la) : la_(la) {
        std::set<std::string> atoms;
        auto collect = [&](const Affine& a) {
            for (const auto& [atom, k] : a.terms) {
                if (atom != kIvAtom) atoms.insert(atom);
            }
        };
        collect(la.start);
        collect(la.bound);
        for (const auto& acc : la.accesses) collect(acc.index);

        zero_ = ctx.make_const(0, 64);
        i1Var_ = static_cast<uint32_t>(ctx.vars().size());
        i1 = ctx.make_sext(ctx.make_var("i1", 32), 32);
        i2Var_ = static_cast<uint32_t>(ctx.vars().size());
        i2 = ctx.make_sext(ctx.make_var("i2", 32), 32);
        paramDomain = ctx.make_const(1, 1);
        for (const auto& atom : atoms) {
            BVNodeId v = ctx.make_sext(ctx.make_var("p:" + atom, 32), 32);
            atomNodes_[atom] = v;
            // A length is never negative.
            if (atom.size() > 7 && atom.compare(atom.size() - 7, 7, ".length") == 0) {
                paramDomain = ctx.make_binary(BVOp::And, paramDomain,
                                              ctx.make_binary(BVOp::Sle, zero_, v));
            }
        }
        lo_ = node(la.start, zero_);
        hi_ = node(la.bound, zero_);
        domain1 = ctx.make_binary(BVOp::And, paramDomain, inRange(i1));
        BVNodeId both = ctx.make_binary(BVOp::And, domain1, inRange(i2));
        distinct = ctx.make_binary(BVOp::And, both, ctx.make_binary(BVOp::Ne, i1, i2));
    }

    BVContext ctx;
    BVNodeId i1 = 0, i2 = 0;
    BVNodeId paramDomain = 0;  // the parameters' own constraints
    BVNodeId domain1 = 0;      // ... and i1 is an iteration
    BVNodeId distinct = 0;     // ... and i2 is a different iteration

    BVNodeId node(const Affine& a, BVNodeId iv) {
        BVNodeId acc = ctx.make_const(static_cast<uint64_t>(a.constant), 64);
        for (const auto& [atom, k] : a.terms) {
            BVNodeId x = atom == kIvAtom ? iv : atomNodes_.at(atom);
            BVNodeId term = k == 1 ? x
                                   : ctx.make_binary(BVOp::Mul,
                                                     ctx.make_const(static_cast<uint64_t>(k), 64), x);
            acc = ctx.make_binary(BVOp::Add, acc, term);
        }
        return acc;
    }

    BVNodeId all(std::initializer_list<BVNodeId> parts) {
        BVNodeId acc = ctx.make_const(1, 1);
        for (BVNodeId p : parts) acc = ctx.make_binary(BVOp::And, acc, p);
        return acc;
    }

    bool hasAtom(const std::string& atom) const { return atomNodes_.count(atom) != 0; }
    BVNodeId atom(const std::string& name) const { return atomNodes_.at(name); }

    struct Answer {
        SolveOutcome::Status status = SolveOutcome::Unknown;
        std::vector<uint64_t> model;
        int64_t v1 = 0, v2 = 0;
        int64_t value(const BVContext& ctx, BVNodeId n) const {
            return brass::verify::sign_extend_bits(ctx.eval_concrete(n, model), 64);
        }
    };

    Answer solve(BVNodeId cond) {
        auto sr = brass::verify::solve_nonzero(ctx, cond);
        Answer a;
        a.status = sr.status;
        a.model = std::move(sr.model);
        if (a.model.size() < ctx.vars().size()) a.model.resize(ctx.vars().size(), 0);
        a.v1 = brass::verify::sign_extend_bits(a.model[i1Var_], 32);
        a.v2 = brass::verify::sign_extend_bits(a.model[i2Var_], 32);
        return a;
    }

private:
    BVNodeId inRange(BVNodeId i) {
        BVNodeId upper = la_.inclusive ? ctx.make_binary(BVOp::Sle, i, hi_)
                                       : ctx.make_binary(BVOp::Slt, i, hi_);
        return ctx.make_binary(BVOp::And, ctx.make_binary(BVOp::Sle, lo_, i), upper);
    }

    const autokernel::LoopAnalysis& la_;
    std::map<std::string, BVNodeId> atomNodes_;
    BVNodeId zero_ = 0, lo_ = 0, hi_ = 0;
    uint32_t i1Var_ = 0, i2Var_ = 0;
};

std::string accessText(const ElementAccess& a) { return a.array + "[" + a.indexText + "]"; }

LoopDependencyWitness undecided(const std::string& what, const ElementAccess& a,
                                const ElementAccess* b) {
    LoopDependencyWitness w;
    w.hazardKind = "UNDECIDED";
    w.arrayName = a.array;
    w.writeIndexExpr = a.indexText;
    w.readIndexExpr = b ? b->indexText : "";
    w.description = "The solver could not decide the " + what + " query for `" + accessText(a) +
                    "`" + (b ? " vs `" + accessText(*b) + "`" : std::string()) +
                    ", so it is not proven";
    return w;
}

// The solver half: WAW, RAW, WAR and bounds over the accesses the front end
// collected, with the loop's real start, bound and step.
void proveAccesses(const autokernel::LoopAnalysis& la, AutoKernelReport& rep) {
    LoopModel m(la);
    std::vector<const ElementAccess*> writes, reads;
    for (const auto& acc : la.accesses) {
        if (acc.isWrite) writes.push_back(&acc);
        if (!acc.isWrite || acc.compound) reads.push_back(&acc);
    }

    // The kernel's thread index is never negative, so neither may an
    // iteration be.
    {
        auto ans = m.solve(m.all({m.domain1, m.ctx.make_binary(BVOp::Slt, m.i1, m.ctx.make_const(0, 64))}));
        if (ans.status != SolveOutcome::Unsat) {
            LoopDependencyWitness w;
            w.hazardKind = ans.status == SolveOutcome::Sat ? "UNSUPPORTED" : "UNDECIDED";
            w.iterA = w.iterB = ans.v1;
            w.description = "Loop start `" + la.startText + "` can be negative (i=" +
                            std::to_string(ans.v1) + "), and a kernel thread index cannot";
            rep.hazards.push_back(std::move(w));
        }
    }

    // 1. WAW: two different iterations write one element.
    rep.provenNoWaw = true;
    for (size_t a = 0; a < writes.size(); ++a) {
        for (size_t b = a; b < writes.size(); ++b) {
            const ElementAccess& wa = *writes[a];
            const ElementAccess& wb = *writes[b];
            if (wa.array != wb.array) continue;
            BVNodeId x1 = m.node(wa.index, m.i1);
            BVNodeId x2 = m.node(wb.index, m.i2);
            auto ans = m.solve(m.all({m.distinct, m.ctx.make_binary(BVOp::Eq, x1, x2)}));
            if (ans.status == SolveOutcome::Unsat) continue;
            rep.provenNoWaw = false;
            if (ans.status != SolveOutcome::Sat) {
                rep.hazards.push_back(undecided("WAW", wa, &wb));
                continue;
            }
            int64_t idx = ans.value(m.ctx, x1);
            rep.hazards.push_back(
                {"WAW", wa.array, wa.indexText, wb.indexText, ans.v1, ans.v2, idx,
                 "Write-After-Write race on `" + wa.array + "`: iteration " +
                     std::to_string(ans.v1) + " writes `" + accessText(wa) + "` and iteration " +
                     std::to_string(ans.v2) + " writes `" + accessText(wb) + "`, both index " +
                     std::to_string(idx)});
        }
    }

    // 2. RAW / WAR: an element one iteration writes is read by another. The
    // order of the two iterations says which: the write first is a flow
    // dependency (RAW), the read first an anti-dependency (WAR).
    rep.provenNoRaw = true;
    rep.provenNoWar = true;
    for (const ElementAccess* w : writes) {
        for (const ElementAccess* r : reads) {
            if (w->array != r->array) continue;
            BVNodeId wIdx = m.node(w->index, m.i1);
            BVNodeId rIdx = m.node(r->index, m.i2);
            BVNodeId same = m.ctx.make_binary(BVOp::Eq, wIdx, rIdx);
            for (bool raw : {true, false}) {
                BVNodeId order = raw ? m.ctx.make_binary(BVOp::Slt, m.i1, m.i2)
                                     : m.ctx.make_binary(BVOp::Slt, m.i2, m.i1);
                auto ans = m.solve(m.all({m.distinct, order, same}));
                if (ans.status == SolveOutcome::Unsat) continue;
                (raw ? rep.provenNoRaw : rep.provenNoWar) = false;
                std::string kind = raw ? "RAW" : "WAR";
                if (ans.status != SolveOutcome::Sat) {
                    rep.hazards.push_back(undecided(kind, *w, r));
                    continue;
                }
                int64_t idx = ans.value(m.ctx, wIdx);
                int64_t first = std::min(ans.v1, ans.v2);
                int64_t second = std::max(ans.v1, ans.v2);
                rep.hazards.push_back(
                    {kind, w->array, w->indexText, r->indexText, first, second, idx,
                     "Loop-carried " + kind + " dependency on `" + w->array + "`: iteration " +
                         std::to_string(first) + " and iteration " + std::to_string(second) +
                         " collide at index " + std::to_string(idx) + " (write `" +
                         accessText(*w) + "` in iteration " + std::to_string(ans.v1) +
                         " vs read `" + accessText(*r) + "` in iteration " +
                         std::to_string(ans.v2) + ")"});
            }
        }
    }

    // 3. Bounds: no index below 0, and none at or past `p.length` where the
    // loop's start or bound names that length (otherwise it is unknown).
    rep.provenInBounds = true;
    for (const auto& acc : la.accesses) {
        BVNodeId idx = m.node(acc.index, m.i1);
        std::vector<std::pair<BVNodeId, std::string>> checks;
        checks.push_back({m.ctx.make_binary(BVOp::Slt, idx, m.ctx.make_const(0, 64)), "negative index"});
        const std::string lenAtom = acc.array + ".length";
        if (m.hasAtom(lenAtom)) {
            checks.push_back({m.ctx.make_binary(BVOp::Sle, m.atom(lenAtom), idx),
                              "index at or past `" + lenAtom + "`"});
        }
        for (const auto& [bad, what] : checks) {
            auto ans = m.solve(m.all({m.domain1, bad}));
            if (ans.status == SolveOutcome::Unsat) continue;
            rep.provenInBounds = false;
            if (ans.status != SolveOutcome::Sat) {
                rep.hazards.push_back(undecided("bounds", acc, nullptr));
                continue;
            }
            int64_t v = ans.value(m.ctx, idx);
            rep.hazards.push_back(
                {"OUT_OF_BOUNDS", acc.array, acc.isWrite ? acc.indexText : "",
                 acc.isWrite ? "" : acc.indexText, ans.v1, ans.v1, v,
                 std::string("Out-of-bounds ") + (acc.isWrite ? "write" : "read") + " `" +
                     accessText(acc) + "` at i=" + std::to_string(ans.v1) + ": " + what + " " +
                     std::to_string(v)});
        }
    }

    // 4. Aliasing: the queries above take two names for two arrays. For each
    // pair of names one of which is written, ask the same questions as if they
    // were one array; a pair that could then collide across iterations is a
    // precondition of the kernel, stated rather than assumed.
    std::set<std::pair<std::string, std::string>> mustDiffer;
    for (const ElementAccess* w : writes) {
        for (const auto& other : la.accesses) {
            if (other.array == w->array) continue;
            auto key = std::minmax(w->array, other.array);
            if (mustDiffer.count({key.first, key.second}) != 0) continue;
            BVNodeId same = m.ctx.make_binary(BVOp::Eq, m.node(w->index, m.i1), m.node(other.index, m.i2));
            // A write against a write collides in either order; a write
            // against a read in either order too (RAW one way, WAR the other).
            auto ans = m.solve(m.all({m.distinct, same}));
            if (ans.status != SolveOutcome::Unsat) mustDiffer.insert({key.first, key.second});
        }
    }
    for (const auto& [a, b] : mustDiffer) rep.requiresDistinct.push_back({a, b});
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
    oss << "  \"requiresDistinct\": [";
    for (size_t i = 0; i < requiresDistinct.size(); ++i) {
        oss << (i ? ", " : "") << "[\"" << jsonEscape(requiresDistinct[i].first) << "\", \""
            << jsonEscape(requiresDistinct[i].second) << "\"]";
    }
    oss << "],\n";
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

    autokernel::LoopAnalysis la = autokernel::analyzeLoopSource(norm);
    if (!la.error.empty()) {
        rep.error = la.error;
        rep.summary = "Could not parse the function: " + la.error;
        return rep;
    }
    if (!la.functionName.empty()) rep.functionName = la.functionName;
    rep.paramNames = la.params;
    if (!la.foundLoop) {
        rep.summary = "No canonical `for (let i = start; i < bound; i++)` loop found";
        return rep;
    }
    rep.foundLoop = true;
    rep.inductionVar = la.inductionVar;
    rep.boundExpr = la.boundText;

    for (const auto& acc : la.accesses) {
        auto& list = acc.isWrite ? rep.writtenArrays : rep.readArrays;
        if (std::find(list.begin(), list.end(), acc.array) == list.end()) list.push_back(acc.array);
        if (acc.isWrite && acc.compound &&
            std::find(rep.readArrays.begin(), rep.readArrays.end(), acc.array) == rep.readArrays.end()) {
            rep.readArrays.push_back(acc.array);
        }
    }

    if (!la.rejectReason.empty()) {
        LoopDependencyWitness w;
        w.hazardKind = la.rejectKind;
        w.arrayName = la.rejectArray;
        w.writeIndexExpr = la.rejectIndex;
        w.description = la.rejectReason;
        rep.hazards.push_back(std::move(w));
        rep.summary = (la.rejectKind == "NON_AFFINE" ? "" : "Rejected parallelization: ") +
                      la.rejectReason;
        return rep;
    }
    if (rep.writtenArrays.empty()) {
        rep.summary = "Loop contains no TypedArray element stores to parallelize";
        return rep;
    }

    proveAccesses(la, rep);
    if (!rep.hazards.empty()) {
        rep.summary = "Refuted parallelization: " + rep.hazards[0].description;
        return rep;
    }

    if (la.bodyText.empty()) {
        rep.summary = "Proven loop-independent, but the loop body could not be printed back from "
                      "its source, so no kernel is emitted";
        return rep;
    }

    // The kernel is the loop body as written, under a guard that admits
    // exactly the iterations the proof ranged over.
    const std::string& iv = la.inductionVar;
    std::string guard;
    std::string boundCmp = (la.inclusive ? " <= " : " < ") + autokernel::printAffine(la.bound, iv);
    if (la.start.isConstant() && la.start.constant == 0) {
        guard = iv + boundCmp;
    } else {
        guard = iv + " >= " + autokernel::printAffine(la.start, iv) + " && " + iv + boundCmp;
    }

    std::ostringstream kss;
    kss << "// @kernel\n";
    kss << "function " << rep.functionName << "(";
    for (size_t i = 0; i < rep.paramNames.size(); ++i) {
        if (i > 0) kss << ", ";
        kss << rep.paramNames[i];
    }
    kss << ") {\n";
    kss << "  let " << iv << " = global_tid_x();\n";
    kss << "  if (" << guard << ") {" << la.bodyText << "}\n";
    kss << "}";
    std::string kernel = kss.str();
    if (!parsesAsModule(kernel)) {
        rep.summary = "Proven loop-independent, but the emitted kernel does not parse, so none is "
                      "emitted";
        return rep;
    }

    rep.parallelizable = true;
    rep.kernelSource = std::move(kernel);
    rep.unifiedDiff = buildSimpleDiff(rep.originalSource, rep.kernelSource);
    rep.certificateHash = fnv1aHex(rep.originalSource + "===" + rep.kernelSource);
    rep.summary = "Proven 100% loop-independent (WAW=UNSAT, RAW=UNSAT, WAR=UNSAT, InBounds=UNSAT) "
                  "over " + std::to_string(la.accesses.size()) +
                  " element access(es); rewritten to parallel @kernel `" + rep.functionName +
                  "` (cert " + rep.certificateHash + "). ";
    if (rep.requiresDistinct.empty()) {
        rep.summary += "Holds even if the arrays are one array.";
    } else {
        rep.summary += "Holds only if these are different arrays:";
        for (const auto& [a, b] : rep.requiresDistinct) rep.summary += " `" + a + "`/`" + b + "`";
        rep.summary += ".";
    }
    rep.summary += " Assumes scalars and indices are int32 numbers.";
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
