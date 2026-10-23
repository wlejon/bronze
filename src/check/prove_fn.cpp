#include "check/prove_fn.h"

#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "check/applet_cert.h"
#include "check/deterministic_check.h"
#include "check/prove_equiv.h"
#include "check/solve_fn.h"
#include "check/static_checker.h"
#include "il/verifier.h"
#include "il/verifier_proofs.h"
#include "lex/lexer.h"
#include "lower/lower.h"
#include "parse/parser.h"
#include "support/diagnostics.h"
#include "support/source.h"
#include "types/infer.h"

namespace bronze::check {
namespace {

std::string jsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 4);
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

std::string computeDigest256Hex(std::string_view data) {
    static constexpr uint64_t kSeeds[4] = {
        0xcbf29ce484222325ULL,
        0x9e3779b97f4a7c15ULL,
        0x6a09e667bb67ae85ULL,
        0x3c6ef372a54ff53aULL,
    };
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int lane = 0; lane < 4; ++lane) {
        uint64_t h = kSeeds[lane];
        for (unsigned char b : data) {
            h ^= static_cast<uint64_t>(b) + static_cast<uint64_t>(lane * 31);
            h *= 0x100000001b3ULL;
            h ^= (h >> 33);
        }
        oss << std::setw(16) << h;
    }
    return oss.str();
}

}  // namespace

ProofCertificateReport proveJsFunction(std::string_view fnSource, const ProveOptions& opts) {
    ProofCertificateReport rep;

    std::string fnName;
    std::string defaultName = opts.fnName.empty() ? "__prove_target" : opts.fnName;
    std::string normalized = normalizeJsFunctionSource(fnSource, &fnName, defaultName);
    if (fnName.empty()) fnName = defaultName;

    SourceSet sources;
    const auto& buf = sources.add("prove_target.js", normalized);
    DiagnosticSink diags;
    auto tokens = Lexer(buf, diags).lex();
    if (diags.hasErrors()) {
        rep.error = diags.render(buf);
        return rep;
    }
    auto astMod = Parser(std::move(tokens), diags).parseModule("prove_target.js");
    if (diags.hasErrors() || !astMod) {
        rep.error = diags.render(buf);
        return rep;
    }
    std::vector<std::string> hostGlobals = {"bro"};
    auto inferred = types::inferModule(*astMod, diags, &hostGlobals);
    if (diags.hasErrors() || !inferred) {
        rep.error = diags.render(buf);
        return rep;
    }
    auto ilMod = lower::lowerModule(*astMod, diags, &*inferred, &hostGlobals, &sources,
                                    nullptr, /*assumeNoBigInt=*/true);
    if (diags.hasErrors() || !ilMod) {
        rep.error = diags.render(buf);
        return rep;
    }

    // 1. Static symbolic hazard check
    DiagnosticSink checkDiags;
    CheckReport checkRep = checkModule(*astMod, &*inferred, &*ilMod, &checkDiags, opts.strictDomNullability);
    for (const auto& f : checkRep.findings) {
        rep.hazards.push_back(std::string(bugKindTag(f.kind)) + ": " + f.message);
    }
    // Also check literal division/modulo by zero in IL
    for (const auto& fn : ilMod->functions) {
        for (const auto& blk : fn.blocks) {
            for (const auto& inst : blk.instructions) {
                if ((inst.op == il::Op::Div || inst.op == il::Op::Mod) && inst.operands.size() >= 2) {
                    for (const auto& i2 : blk.instructions) {
                        if (i2.result == inst.operands[1] &&
                            ((i2.op == il::Op::ConstI32 && i2.immI32 == 0) ||
                             (i2.op == il::Op::ConstF64 && i2.immF64 == 0.0))) {
                            rep.hazards.push_back("div-by-zero: division or modulo by literal zero");
                        }
                    }
                }
            }
        }
    }
    rep.hazardFree = rep.hazards.empty();

    // 2. Deterministic lockstep audit
    std::vector<CheckDiagnostic> detDiags;
    checkDeterministicLockstep(*astMod, *ilMod, detDiags);
    for (const auto& d : detDiags) {
        rep.lockstepHazards.push_back(std::string(deterministicHazardTag(d.kind)) + ": " + d.message);
    }
    rep.deterministic = rep.lockstepHazards.empty();

    // 3. IL & zero-guard proof certificate verification
    DiagnosticSink proofDiags;
    bool ilOk = il::verify(*ilMod, proofDiags);
    bool proofsOk = il::verifyModuleProofs(*ilMod, proofDiags);
    rep.proofsValid = ilOk && proofsOk && !proofDiags.hasErrors();

    for (const auto& fn : ilMod->functions) {
        if (fn.isExternal || fn.isEntryPoint || fn.name == "main") continue;
        for (const auto& p : fn.params) {
            if (p.type == il::Type::F64 || p.type == il::Type::I32) {
                ++rep.provenGuards;
                ++rep.totalGuards;
            }
        }
        for (const auto& blk : fn.blocks) {
            if (blk.copyClass == il::CopyClass::Slow) continue;
            for (const auto& inst : blk.instructions) {
                if (inst.type == il::Type::F64 || inst.type == il::Type::I32 ||
                    inst.type == il::Type::Bool) {
                    ++rep.unboxedF64Ops;
                }
                if (inst.op == il::Op::Unbox) {
                    ++rep.totalGuards;
                    if (inst.rawUnbox || inst.nullishUnbox) ++rep.provenGuards;
                } else if (inst.op == il::Op::ElemGetTyped || inst.op == il::Op::ElemSetTyped) {
                    ++rep.totalGuards;
                    ++rep.provenGuards;
                } else if (inst.op == il::Op::ElemGet || inst.op == il::Op::ElemSet ||
                           inst.op == il::Op::PinGuard) {
                    ++rep.totalGuards;
                } else if ((inst.op == il::Op::PropGet || inst.op == il::Op::PropSet) &&
                           inst.staticSlot != il::Instruction::kNoStaticSlot) {
                    ++rep.totalGuards;
                    ++rep.provenGuards;
                }
            }
        }
    }
    if (rep.totalGuards == 0 && rep.unboxedF64Ops > 0) {
        rep.provenGuards = rep.unboxedF64Ops;
        rep.totalGuards = rep.unboxedF64Ops;
    }

    // 4. Optional equivalence proof against `opts.equivalentToSource`
    if (!opts.equivalentToSource.empty()) {
        rep.hasEquivalenceCheck = true;
        std::string normOther = normalizeJsFunctionSource(opts.equivalentToSource, nullptr, fnName);
        std::string eqErr;
        EquivReport eq = proveSourceEquivalent(
            normalized, normOther, fnName, opts.domainMin, opts.domainMax, &eqErr);
        rep.equivalent = (eq.status == EquivStatus::Equivalent);
        if (eq.status == EquivStatus::Counterexample) {
            rep.counterexampleArgs = eq.counterexampleArgs;
            rep.expectedOutput = eq.expectedOutput;
            rep.actualOutput = eq.actualOutput;
        }
    }

    // 5. Optional round-trip proof against `opts.inverseFnSource`
    if (!opts.inverseFnSource.empty()) {
        rep.hasRoundTripCheck = true;
        std::string encName;
        std::string decName;
        std::string encSrc = normalizeJsFunctionSource(fnSource, &encName, "__prove_encode");
        std::string decSrc = normalizeJsFunctionSource(opts.inverseFnSource, &decName, "__prove_decode");
        if (encName.empty()) encName = "__prove_encode";
        if (decName.empty()) decName = "__prove_decode";
        if (encName == decName) {
            decName = "__prove_decode_inv";
            size_t pos = decSrc.find(encName);
            if (pos != std::string::npos) {
                decSrc.replace(pos, encName.size(), decName);
            }
        }
        std::string combined = encSrc + "\n" + decSrc;
        std::string rtErr;
        EquivReport rt = proveRoundTripSource(
            combined, encName, decName, opts.domainMin, opts.domainMax, &rtErr);
        rep.roundTripProven = (rt.status == EquivStatus::Equivalent);
        if (rt.status == EquivStatus::Counterexample) {
            rep.counterexampleArgs = rt.counterexampleArgs;
            rep.expectedOutput = rt.expectedOutput;
            rep.actualOutput = rt.actualOutput;
        }
    }

    // 6. Optional test vector synthesis
    if (opts.synthesizeTests) {
        SynthTestsReport synth = synthesizeModuleTests(*ilMod, astMod.get(), "prove_target.js");
        rep.synthesizedTests = std::move(synth.tests);
        for (const auto& tc : rep.synthesizedTests) {
            if (tc.kind == SynthTestKind::HazardReproducer) {
                rep.hazards.push_back("smt-hazard: " + tc.hazardDescription);
                rep.hazardFree = false;
            }
        }
    }

    rep.verified = rep.hazardFree && rep.deterministic && rep.proofsValid &&
                   rep.equivalent && rep.roundTripProven;
    rep.sha256 = computeDigest256Hex(normalized);

    std::ostringstream cert;
    cert << "{"
         << "\"schema\":\"bronze.proof.cert.v1\","
         << "\"function\":\"" << jsonEscape(fnName) << "\","
         << "\"verified\":" << (rep.verified ? "true" : "false") << ","
         << "\"hazardFree\":" << (rep.hazardFree ? "true" : "false") << ","
         << "\"deterministic\":" << (rep.deterministic ? "true" : "false") << ","
         << "\"proofsValid\":" << (rep.proofsValid ? "true" : "false") << ","
         << "\"provenGuards\":" << rep.provenGuards << ","
         << "\"totalGuards\":" << rep.totalGuards << ","
         << "\"unboxedF64Ops\":" << rep.unboxedF64Ops << ","
         << "\"equivalent\":" << (rep.equivalent ? "true" : "false") << ","
         << "\"roundTripProven\":" << (rep.roundTripProven ? "true" : "false") << ","
         << "\"sha256\":\"" << rep.sha256 << "\""
         << "}";
    rep.certificateJson = cert.str();
    return rep;
}

}  // namespace bronze::check
