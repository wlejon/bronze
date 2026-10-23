#include "check/applet_cert.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "il/verifier.h"
#include "il/verifier_proofs.h"
#include "lower/lower.h"
#include "modules/modules.h"
#include "support/diagnostics.h"
#include "types/infer.h"

namespace bronze::check {
namespace {

bool isSafeEcmaBuiltin(std::string_view name) {
    static const std::unordered_set<std::string_view> kSafe = {
        "Math", "Number", "String", "Boolean", "Array", "Object", "JSON",
        "Map", "Set", "WeakMap", "WeakSet", "WeakRef", "FinalizationRegistry",
        "Promise", "Symbol", "BigInt", "RegExp", "Date", "Intl", "Iterator",
        "Reflect", "Proxy",
        "Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError",
        "URIError", "AggregateError",
        "ArrayBuffer", "SharedArrayBuffer", "DataView", "Atomics",
        "Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array", "Uint16Array",
        "Int32Array", "Uint32Array", "Float16Array", "Float32Array", "Float64Array",
        "BigInt64Array", "BigUint64Array",
        "isNaN", "isFinite", "parseInt", "parseFloat",
        "encodeURI", "encodeURIComponent", "decodeURI", "decodeURIComponent",
        "escape", "unescape",
        "NaN", "Infinity", "undefined", "globalThis",
        "performance", "console",
        "__bronze_module_publish", "__bronze_module_lookup"
    };
    if (name.rfind("console.", 0) == 0) return true;
    return kSafe.count(name) != 0;
}

bool isPrivilegedCapability(std::string_view cap) {
    return cap == "child_process" || cap == "fs" || cap == "process" ||
           cap == "require" || cap == "eval" || cap == "Function" ||
           cap == "bro.sys" || cap == "bro.cred" || cap == "bro.seat" ||
           cap == "bro.portal" || cap == "bro.displays" ||
           cap == "bro.compositor" || cap == "bro.wl" ||
           cap == "bro.fs" || cap == "bro.process" ||
           cap.rfind("__bro_native", 0) == 0;
}

// Extract a string value `"key": "val"` from JSON text.
std::string extractJsonStringField(const std::string& json, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return {};
    size_t colon = json.find(':', pos + pat.size());
    if (colon == std::string::npos) return {};
    size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return {};
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return json.substr(q1 + 1, q2 - q1 - 1);
}

// Extract a string array `"key": ["a", "b"]` from JSON text.
bool extractJsonStringArray(const std::string& json, const std::string& key,
                            std::vector<std::string>& out) {
    std::string pat = "\"" + key + "\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return false;
    size_t colon = json.find(':', pos + pat.size());
    if (colon == std::string::npos) return false;
    size_t openBr = json.find('[', colon + 1);
    if (openBr == std::string::npos) return false;
    size_t closeBr = json.find(']', openBr + 1);
    if (closeBr == std::string::npos) return false;

    size_t cur = openBr + 1;
    while (cur < closeBr) {
        size_t q1 = json.find('"', cur);
        if (q1 == std::string::npos || q1 >= closeBr) break;
        size_t q2 = json.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 >= closeBr) break;
        out.push_back(json.substr(q1 + 1, q2 - q1 - 1));
        cur = q2 + 1;
    }
    return true;
}

bool readFileBytes(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

}  // namespace

std::string computeFnv1a64Hex(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= 0x100000001b3ULL;
    }
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << hash;
    return oss.str();
}

std::string computeFileFnv1a64Hex(const std::string& filePath, bool* okOut) {
    std::string bytes;
    if (!readFileBytes(filePath, bytes)) {
        if (okOut) *okOut = false;
        return {};
    }
    if (okOut) *okOut = true;
    return computeFnv1a64Hex(bytes.data(), bytes.size());
}

AppletCertResult verifyAndEmitAppletCert(
    const std::string& entryOrDirPath,
    const AppletCertOptions& opts) {
    namespace fs = std::filesystem;
    AppletCertResult res;

    std::error_code ec;
    fs::path inputPath(entryOrDirPath);
    fs::path appDir;
    fs::path entryFile;

    if (fs::is_directory(inputPath, ec)) {
        appDir = inputPath;
        fs::path broJson = !opts.broJsonPath.empty() ? fs::path(opts.broJsonPath) : (appDir / "bro.json");
        std::string mainRel = "main.js";
        if (fs::exists(broJson, ec)) {
            std::string jsonText;
            if (readFileBytes(broJson.string(), jsonText)) {
                std::string m = extractJsonStringField(jsonText, "main");
                if (!m.empty()) mainRel = m;
            }
        }
        entryFile = appDir / mainRel;
        if (!fs::exists(entryFile, ec) && fs::exists(appDir / "index.js", ec)) {
            entryFile = appDir / "index.js";
        }
    } else {
        entryFile = inputPath;
        appDir = inputPath.parent_path();
        if (appDir.empty()) appDir = ".";
    }

    if (!fs::exists(entryFile, ec)) {
        res.errorMessage = "error: applet entry file not found: " + entryFile.string() + "\n";
        return res;
    }

    // Read bro.json capability manifest if present
    fs::path broJsonPath = !opts.broJsonPath.empty() ? fs::path(opts.broJsonPath) : (appDir / "bro.json");
    bool hasExplicitManifestCaps = !opts.allowedCapabilities.empty();
    std::set<std::string> allowedCaps(opts.allowedCapabilities.begin(), opts.allowedCapabilities.end());
    if (fs::exists(broJsonPath, ec)) {
        std::string jsonText;
        if (readFileBytes(broJsonPath.string(), jsonText)) {
            std::vector<std::string> caps;
            if (extractJsonStringArray(jsonText, "capabilities", caps)) {
                hasExplicitManifestCaps = true;
                allowedCaps.insert(caps.begin(), caps.end());
            }
            std::vector<std::string> perms;
            if (extractJsonStringArray(jsonText, "permissions", perms)) {
                hasExplicitManifestCaps = true;
                allowedCaps.insert(perms.begin(), perms.end());
            }
        }
    }

    // 1. Load, infer, and lower the applet program (admitting `bro` as a host global so
    //    `bro.ui` / `bro.sys` accesses lower to GlobalGet("bro") + PropGet without warning)
    std::vector<std::string> hostGlobals = {"bro"};
    for (const auto& c : allowedCaps) {
        size_t dot = c.find('.');
        hostGlobals.push_back(dot == std::string::npos ? c : c.substr(0, dot));
    }

    SourceSet sources;
    DiagnosticSink diags;
    auto astMod = modules::loadProgram(entryFile.string(), sources, diags);
    if (!astMod || diags.hasErrors()) {
        res.errorMessage = diags.render(sources);
        return res;
    }

    auto inferred = types::inferModule(*astMod, diags, &hostGlobals);
    if (diags.hasErrors() || !inferred) {
        res.errorMessage = diags.render(sources);
        return res;
    }

    auto ilMod = lower::lowerModule(*astMod, diags, &*inferred, &hostGlobals, &sources,
                                    nullptr, /*assumeNoBigInt=*/true);
    if (diags.hasErrors() || !ilMod) {
        res.errorMessage = diags.render(sources);
        return res;
    }

    // 2. Run Static Symbolic Hazard Checker (`NullDeref`, `OutOfBounds`, `NaNPoison`) + `DivByZero`
    CheckReport checkRep = checkModule(*astMod, &*inferred, &*ilMod, &diags);
    for (const auto& f : checkRep.findings) {
        HazardReport hz;
        if (f.kind == BugKind::NullDeref) hz.kind = HazardKind::NullDeref;
        else if (f.kind == BugKind::OutOfBounds) hz.kind = HazardKind::OutOfBounds;
        else hz.kind = HazardKind::NaNPoison;
        hz.span = f.span;
        hz.message = f.message;
        uint16_t fIdx = f.span.file;
        if (fIdx < ilMod->sourceFiles.size()) hz.file = ilMod->sourceFiles[fIdx];
        else hz.file = entryFile.string();
        if (fIdx < ilMod->lineTables.size()) {
            SourceBuffer::LineCol lc = ilMod->lineTables[fIdx].lineCol(f.span.begin);
            hz.line = lc.line;
            hz.col = lc.column;
        }
        res.hazards.push_back(std::move(hz));
    }

    // Check for unguarded literal division/modulo by zero (`DivByZero`)
    for (const auto& fn : ilMod->functions) {
        std::unordered_set<il::ValueId> zeroVals;
        for (const auto& blk : fn.blocks) {
            for (const auto& inst : blk.instructions) {
                if (inst.op == il::Op::ConstF64 && inst.immF64 == 0.0) {
                    zeroVals.insert(inst.result);
                } else if (inst.op == il::Op::ConstI32 && inst.immI32 == 0) {
                    zeroVals.insert(inst.result);
                } else if ((inst.op == il::Op::Unbox || inst.op == il::Op::Box) &&
                           !inst.operands.empty() && zeroVals.count(inst.operands[0])) {
                    zeroVals.insert(inst.result);
                } else if ((inst.op == il::Op::Div || inst.op == il::Op::Mod) &&
                           inst.operands.size() >= 2 && zeroVals.count(inst.operands[1])) {
                    HazardReport hz;
                    hz.kind = HazardKind::DivByZero;
                    hz.functionName = fn.displayName.empty() ? fn.name : fn.displayName;
                    hz.span = inst.span;
                    uint16_t fIdx = inst.span.file < ilMod->sourceFiles.size() ? inst.span.file : fn.sourceFile;
                    if (fIdx < ilMod->sourceFiles.size()) hz.file = ilMod->sourceFiles[fIdx];
                    if (fIdx < ilMod->lineTables.size()) {
                        SourceBuffer::LineCol lc = ilMod->lineTables[fIdx].lineCol(inst.span.begin);
                        hz.line = lc.line;
                        hz.col = lc.column;
                    }
                    hz.message = "potential division or modulo by zero in '" + hz.functionName + "'";
                    res.hazards.push_back(std::move(hz));
                }
            }
        }
    }
    res.hazardCount = static_cast<uint32_t>(res.hazards.size());

    // 3. Verify IL & Machine-Checkable Proof Certificates (#67)
    DiagnosticSink proofDiags;
    bool ilValid = il::verify(*ilMod, proofDiags);
    bool proofsOk = il::verifyModuleProofs(*ilMod, proofDiags);
    res.proofsValid = ilValid && proofsOk && !proofDiags.hasErrors();

    // 4. Audit Host Globals & Capabilities (`bro.*`, `child_process`, `fs`, etc.)
    std::set<std::string> referencedCaps;
    for (const auto& ni : ilMod->nativeImports) {
        referencedCaps.insert(ni.name);
    }
    for (const auto& fn : ilMod->functions) {
        std::unordered_map<il::ValueId, std::string> globalSyms;
        std::unordered_set<il::ValueId> usedAsObjectReceiver;
        for (const auto& blk : fn.blocks) {
            for (const auto& inst : blk.instructions) {
                if ((inst.op == il::Op::GlobalGet || inst.op == il::Op::ResolveName) &&
                    inst.keyIndex < ilMod->keyConstants.size()) {
                    const std::string& gname = ilMod->keyConstants[inst.keyIndex];
                    globalSyms[inst.result] = gname;
                } else if ((inst.op == il::Op::PropGet || inst.op == il::Op::MethodCall) &&
                           !inst.operands.empty() && inst.keyIndex < ilMod->keyConstants.size()) {
                    il::ValueId recv = inst.operands[0];
                    auto it = globalSyms.find(recv);
                    if (it != globalSyms.end()) {
                        usedAsObjectReceiver.insert(recv);
                        std::string sub = it->second + "." + ilMod->keyConstants[inst.keyIndex];
                        globalSyms[inst.result] = sub;
                        if (it->second == "bro" || !isSafeEcmaBuiltin(it->second)) {
                            referencedCaps.insert(sub);
                        }
                    }
                }
            }
        }
        for (const auto& [vid, gname] : globalSyms) {
            if (gname == "bro" && usedAsObjectReceiver.count(vid)) continue;
            if (!isSafeEcmaBuiltin(gname)) {
                referencedCaps.insert(gname);
            }
        }
    }

    res.capabilities.assign(referencedCaps.begin(), referencedCaps.end());
    for (const auto& cap : res.capabilities) {
        bool allowed = false;
        if (allowedCaps.count(cap) != 0) {
            allowed = true;
        } else {
            // Check prefix match, e.g. `bro.ui` allows `bro.ui.createWindow`
            for (const auto& ac : allowedCaps) {
                if (cap.size() > ac.size() && cap.compare(0, ac.size(), ac) == 0 &&
                    cap[ac.size()] == '.') {
                    allowed = true;
                    break;
                }
            }
        }
        if (!allowed && (isPrivilegedCapability(cap) || hasExplicitManifestCaps)) {
            res.unauthorizedCapabilities.push_back(cap);
        }
    }

    if (res.hazardCount > 0 || !res.proofsValid || !res.unauthorizedCapabilities.empty()) {
        std::ostringstream err;
        for (const auto& h : res.hazards) {
            err << h.file << ":" << h.line << ":" << h.col << ": error ["
                << hazardKindName(h.kind) << "]: " << h.message << "\n";
        }
        if (!res.proofsValid) {
            err << proofDiags.render(sources);
            if (err.str().empty()) err << "error: IL proof certificate verification failed\n";
        }
        for (const auto& ucap : res.unauthorizedCapabilities) {
            err << entryFile.string()
                << ":1:1: error [UnauthorizedCapability]: applet uses unauthorized capability or host global '"
                << ucap << "'\n";
        }
        res.errorMessage = err.str();
        return res;
    }

    // 5. Compute 64-bit FNV-1a hash of the compiled module (.so) or source file
    fs::path hashTarget = !opts.soPath.empty() ? fs::path(opts.soPath) : (appDir / "app.so");
    if (!fs::exists(hashTarget, ec)) {
        hashTarget = entryFile;
    }
    bool hashOk = false;
    res.moduleHash = computeFileFnv1a64Hex(hashTarget.string(), &hashOk);
    if (!hashOk) {
        res.errorMessage = "error: cannot hash module file " + hashTarget.string() + "\n";
        return res;
    }

    res.verified = true;

    std::ostringstream json;
    json << "{\n"
         << "  \"verified\": true,\n"
         << "  \"hazards\": " << res.hazardCount << ",\n"
         << "  \"proofsValid\": " << (res.proofsValid ? "true" : "false") << ",\n"
         << "  \"moduleHash\": \"" << res.moduleHash << "\",\n"
         << "  \"capabilities\": [";
    for (size_t i = 0; i < res.capabilities.size(); ++i) {
        if (i > 0) json << ", ";
        json << "\"" << res.capabilities[i] << "\"";
    }
    json << "]\n}\n";
    res.certJson = json.str();

    fs::path outPath = !opts.certOutPath.empty() ? fs::path(opts.certOutPath) : (appDir / "app.cert.json");
    res.certFilePath = outPath.string();
    std::ofstream out(outPath, std::ios::binary);
    if (!out) {
        res.verified = false;
        res.errorMessage = "error: cannot write applet certificate " + outPath.string() + "\n";
        return res;
    }
    out.write(res.certJson.data(), static_cast<std::streamsize>(res.certJson.size()));
    return res;
}

int runVerifyAppletCli(int argc, char** argv, std::string* outString) {
    auto reportErr = [&](const std::string& msg) {
        if (outString) *outString = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    };

    std::string targetPath;
    AppletCertOptions opts;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--so") {
            if (i + 1 >= argc) return reportErr("error: missing argument for --so\n");
            opts.soPath = argv[++i];
        } else if (arg.rfind("--so=", 0) == 0) {
            opts.soPath = arg.substr(5);
        } else if (arg == "--manifest") {
            if (i + 1 >= argc) return reportErr("error: missing argument for --manifest\n");
            opts.broJsonPath = argv[++i];
        } else if (arg == "-o") {
            if (i + 1 >= argc) return reportErr("error: missing argument for -o\n");
            opts.certOutPath = argv[++i];
        } else if (targetPath.empty()) {
            targetPath = arg;
        } else {
            return reportErr("error: unexpected argument " + arg + "\n");
        }
    }

    if (targetPath.empty()) {
        return reportErr("error: missing <appdir_or_entry.js> for verify-applet\n");
    }

    AppletCertResult res = verifyAndEmitAppletCert(targetPath, opts);
    if (!res.verified) {
        return reportErr(res.errorMessage);
    }

    if (outString) *outString = res.certJson;
    else std::fputs(res.certJson.c_str(), stdout);
    return 0;
}

}  // namespace bronze::check
