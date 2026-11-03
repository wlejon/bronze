// Printing the census (census.h): totals, the per-class counts, the top
// causes of what stayed unresolved, a per-function table, and, for the
// functions a filter names, every site.

#include <algorithm>
#include <cstdio>
#include <map>
#include <unordered_map>

#include "abi/bronze_abi.h"
#include "opt/wpo/census.h"

namespace bronze::wpo {

namespace {

std::string fmt(const char* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

struct Tally {
    std::map<std::string, size_t> counts;
    void add(const std::string& k) { ++counts[k]; }
    std::string top(const std::string& prefix, const std::string& title, size_t n) const {
        std::vector<std::pair<size_t, std::string>> rows;
        for (const auto& [k, c] : counts) rows.push_back({c, k});
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : a.second < b.second;
        });
        std::string out = prefix + ": " + title + " (" + std::to_string(rows.size()) + " distinct)\n";
        for (size_t i = 0; i < rows.size() && i < n; ++i) {
            out += "  " + std::to_string(rows[i].first) + "  " + rows[i].second + "\n";
        }
        return out;
    }
};

// Per-function counters, one row of the table.
struct FnRow {
    size_t get = 0, getSingle = 0, getSameSlot = 0, getEscaped = 0, getUnknown = 0, getOther = 0;
    size_t set = 0, setSlot = 0, setUnknown = 0;
    size_t idx = 0, idxResolved = 0;
    size_t call = 0, callSingle = 0, callMulti = 0, callBuiltin = 0, callUnknown = 0;
    size_t stamped = 0, direct = 0;
};

}  // namespace

std::string formatCensus(Solver& solver, const ShapeAnalysis& shapes, const std::vector<SiteFact>& facts,
                         const CensusOptions& opts, double shapeMillis) {
    const auto& mod = solver.module();
    std::string out;

    // ---- totals ----
    size_t reachable = 0, defined = 0;
    for (uint32_t f = 0; f < mod.functions.size(); ++f) {
        if (mod.functions[f].isExternal) continue;
        ++defined;
        if (solver.reachable(f)) ++reachable;
    }
    std::map<std::string, size_t> byKind;
    size_t escaped = 0, modelled = 0, exact = 0, single = 0;
    Tally escapeReasons, escapeSites, escapedInstances, inexactWhy;
    for (ObjId id = 0; id < solver.objectCount(); ++id) {
        const AObj& o = solver.object(id);
        ++byKind[objKindName(o.kind)];
        if (o.escaped) {
            ++escaped;
            const std::string why = o.escapeReason == UINT32_MAX ? std::string("?") : solver.reason(o.escapeReason);
            escapeReasons.add(std::string(objKindName(o.kind)) + ": " + why);
            std::string where = "(no instruction)";
            if (o.escapeSite != UINT32_MAX) {
                const auto loc = solver.insts().locate(o.escapeSite);
                const auto& in = mod.functions[loc.fn].blocks[loc.block].instructions[loc.inst];
                where = mod.functions[loc.fn].name + " " + il::opName(in.op);
                if (in.op == il::Op::PropGet || in.op == il::Op::PropSet || in.op == il::Op::MethodCall) {
                    where += " ." + solver.keys().name(in.keyIndex);
                }
            }
            escapeSites.add(why + " at " + where);
            const bool classFn = o.kind == ObjKind::Function && o.fn < mod.functions.size() &&
                                 (mod.functions[o.fn].fnFlags & BRONZE_ABI_FN_FLAG_CLASS_CTOR) != 0;
            if (o.kind == ObjKind::Instance || classFn) {
                escapedInstances.add(solver.describe(id) + ": " + why + " at " + where);
            }
        }
        const ObjShape& sh = shapes.shape(id);
        if (sh.modelled) {
            ++modelled;
            if (sh.exact) ++exact;
            else inexactWhy.add(std::string(objKindName(o.kind)) + ": " + sh.why);
            if (shapes.singleShape(id)) ++single;
        }
    }
    out += opts.prefix + ": functions " + std::to_string(reachable) + " reachable of " + std::to_string(defined) +
           "; abstract objects " + std::to_string(solver.objectCount()) + " (";
    bool firstKind = true;
    for (const auto& [k, n] : byKind) {
        out += (firstKind ? "" : ", ") + k + " " + std::to_string(n);
        firstKind = false;
    }
    out += "), escaped " + std::to_string(escaped) + "\n";
    const auto& st = solver.stats();
    out += opts.prefix + ": solver " + std::to_string(st.transfers) + " instruction transfers, " + fmt("%.0f", st.millis) + " ms; shapes " + fmt("%.0f", shapeMillis) +
           " ms; widenings " + std::to_string(st.widenings) + "\n";
    out += opts.prefix + ": shapes modelled " + std::to_string(modelled) + ", exact " + std::to_string(exact) +
           ", single-shape " + std::to_string(single) + "\n";

    // ---- per-class counts ----
    std::map<std::string, std::map<std::string, size_t>> classes;
    size_t stampAgree = 0, stampDisagree = 0, directAgree = 0, directDisagree = 0, directUnresolved = 0;
    Tally unknownCauses, escapedSites, notInPrefix, unknownTargets, mixedCauses, disagreements;
    std::vector<FnRow> rows(mod.functions.size());
    for (const auto& f : facts) {
        FnRow& row = rows[f.fn];
        if (f.stamped) ++row.stamped;
        if (f.direct) ++row.direct;
        if (f.isCall) {
            classes["method.call"][callClassName(f.call)]++;
            ++row.call;
            switch (f.call) {
                case CallClass::Single: ++row.callSingle; break;
                case CallClass::Multi: ++row.callMulti; break;
                case CallClass::Builtin: ++row.callBuiltin; break;
                case CallClass::Unknown: case CallClass::UnknownTarget: case CallClass::Mixed: ++row.callUnknown; break;
                default: break;
            }
            if (f.call == CallClass::Unknown) unknownCauses.add("call recv: " + f.cause);
            if (f.call == CallClass::UnknownTarget) {
                unknownTargets.add(f.escapeWhy.empty() ? f.cause : "escaped " + f.escapeWhy);
            }
            if (f.direct) {
                if (f.call == CallClass::Single && f.targets.size() == 1) {
                    if (f.targets[0] == f.directTarget) {
                        ++directAgree;
                    } else {
                        ++directDisagree;
                        disagreements.add(mod.functions[f.fn].name + " b" + std::to_string(f.block) + ":" +
                                          std::to_string(f.inst) + " ." + solver.keys().name(f.key) + " direct " +
                                          mod.functions[f.directTarget].name + " vs " +
                                          mod.functions[f.targets[0]].name + " {" + f.detail + "}");
                    }
                } else {
                    ++directUnresolved;
                }
            }
            continue;
        }
        const bool index = solver.keys().isIndex(f.key);
        const std::string group = index ? "prop index" : f.op == il::Op::PropGet ? "prop.get" : "prop.set";
        classes[group][propClassName(f.prop)]++;
        if (f.stamped && !f.guessed) classes[group]["(stamped proven)"]++;
        if (f.stamped && f.guessed) classes[group]["(stamped guess)"]++;
        if (f.prop == PropClass::Unknown) unknownCauses.add((f.op == il::Op::PropGet ? "get recv: " : "set recv: ") + f.cause);
        if (f.prop == PropClass::SameSlotEscaped) escapedSites.add(f.escapeWhy);
        if (f.prop == PropClass::NotInPrefix) notInPrefix.add(f.cause);
        if (f.prop == PropClass::Mixed && !f.cause.empty()) mixedCauses.add(f.cause);
        const bool resolvedSlot = f.prop == PropClass::SingleShape || f.prop == PropClass::SameSlot ||
                                  f.prop == PropClass::SameSlotEscaped;
        if (f.stamped && resolvedSlot) {
            if (f.stampSlot == f.slot) {
                ++stampAgree;
            } else {
                ++stampDisagree;
                disagreements.add(mod.functions[f.fn].name + " b" + std::to_string(f.block) + ":" +
                                  std::to_string(f.inst) + " ." + solver.keys().name(f.key) + " stamp " +
                                  (f.guessed ? "guess " : "") + std::to_string(f.stampSlot) + " vs " +
                                  std::to_string(f.slot) + " {" + f.detail + "}");
            }
        }
        if (index) {
            ++row.idx;
            if (f.prop == PropClass::IndexArray || f.prop == PropClass::IndexTyped) ++row.idxResolved;
        } else if (f.op == il::Op::PropGet) {
            ++row.get;
            if (f.prop == PropClass::SingleShape) ++row.getSingle;
            else if (f.prop == PropClass::SameSlot) ++row.getSameSlot;
            else if (f.prop == PropClass::SameSlotEscaped) ++row.getEscaped;
            else if (f.prop == PropClass::Unknown) ++row.getUnknown;
            else ++row.getOther;
        } else {
            ++row.set;
            if (resolvedSlot) ++row.setSlot;
            else if (f.prop == PropClass::Unknown) ++row.setUnknown;
        }
    }
    for (const auto& [group, counts] : classes) {
        size_t total = 0;
        for (const auto& [k, n] : counts) {
            if (k[0] != '(') total += n;
        }
        out += opts.prefix + ": " + group + " sites " + std::to_string(total) + ":";
        for (const auto& [k, n] : counts) out += " " + k + " " + std::to_string(n) + ";";
        out += "\n";
    }
    out += opts.prefix + ": static-slot stamps vs the analysis's slot: agree " + std::to_string(stampAgree) + ", disagree " +
           std::to_string(stampDisagree) + "\n";
    out += opts.prefix + ": direct method targets vs the analysis: agree " + std::to_string(directAgree) + ", disagree " +
           std::to_string(directDisagree) + ", not single " + std::to_string(directUnresolved) + "\n";
    out += disagreements.top(opts.prefix, "sites where lowering's stamp or direct target and the analysis disagree", 20);
    out += unknownCauses.top(opts.prefix, "unknown-receiver causes", 40);
    out += unknownTargets.top(opts.prefix, "unknown method targets", 20);
    out += escapedSites.top(opts.prefix, "same-slot-escaped sites by escaped receiver", 20);
    out += notInPrefix.top(opts.prefix, "not-in-prefix sites by reason", 20);
    out += mixedCauses.top(opts.prefix, "mixed sites with an unknown prototype", 10);
    out += escapeReasons.top(opts.prefix, "escaped objects by kind and reason", 25);
    out += escapeSites.top(opts.prefix, "escaped objects by the instruction that let them go", 30);
    out += escapedInstances.top(opts.prefix, "escaped classes and class instances", 400);
    out += inexactWhy.top(opts.prefix, "inexact shapes by reason", 20);

    // ---- per-function table ----
    out += opts.prefix + ": per function: get <n> (single same-slot escaped-slot unknown other) | set <n> (slot unknown) | "
           "idx <resolved>/<n> | call <n> (single multi builtin unknown) | stamped direct\n";
    for (uint32_t f = 0; f < mod.functions.size(); ++f) {
        const FnRow& r = rows[f];
        if (!solver.reachable(f) || r.get + r.set + r.idx + r.call == 0) continue;
        out += opts.prefix + "-fn " + mod.functions[f].name + " | get " + std::to_string(r.get) + " (" +
               std::to_string(r.getSingle) + " " + std::to_string(r.getSameSlot) + " " +
               std::to_string(r.getEscaped) + " " + std::to_string(r.getUnknown) + " " +
               std::to_string(r.getOther) + ") | set " + std::to_string(r.set) + " (" +
               std::to_string(r.setSlot) + " " + std::to_string(r.setUnknown) + ") | idx " +
               std::to_string(r.idxResolved) + "/" + std::to_string(r.idx) + " | call " +
               std::to_string(r.call) + " (" + std::to_string(r.callSingle) + " " +
               std::to_string(r.callMulti) + " " + std::to_string(r.callBuiltin) + " " +
               std::to_string(r.callUnknown) + ") | " + std::to_string(r.stamped) + " " +
               std::to_string(r.direct) + "\n";
    }

    // ---- per-site dump ----
    if (opts.perSite) {
        std::vector<std::string> parts;
        for (size_t at = 0; at <= opts.filter.size();) {
            const size_t comma = std::min(opts.filter.find(',', at), opts.filter.size());
            parts.push_back(opts.filter.substr(at, comma - at));
            at = comma + 1;
        }
        for (const auto& f : facts) {
            const auto& name = mod.functions[f.fn].name;
            const bool named = std::any_of(parts.begin(), parts.end(), [&](const std::string& p) {
                return name.find(p) != std::string::npos;
            });
            if (!named) continue;
            std::string line = opts.prefix + "-site " + name + " b" + std::to_string(f.block) + ":" + std::to_string(f.inst) +
                               " " + il::opName(f.op) + " ." + solver.keys().name(f.key) +
                               (f.inlined ? " [inlined]" : "") + " -> " +
                               (f.isCall ? callClassName(f.call) : propClassName(f.prop));
            if (f.slot >= 0) line += " slot " + std::to_string(f.slot);
            if (f.holders > 0) line += " holders " + std::to_string(f.holders);
            line += " recv " + std::to_string(f.receivers) + " {" + f.detail + "}";
            if (f.isCall && !f.targets.empty()) {
                line += " targets {";
                for (size_t i = 0; i < f.targets.size() && i < 4; ++i) {
                    line += (i ? ", " : "") + mod.functions[f.targets[i]].name;
                }
                if (f.targets.size() > 4) line += ", +" + std::to_string(f.targets.size() - 4);
                line += "}";
            }
            if (f.stamped) line += std::string(" stamp ") + (f.guessed ? "guess " : "") + std::to_string(f.stampSlot);
            if (f.mono) line += " mono";
            if (!f.wpoStamp.empty()) line += " wpo-stamp " + f.wpoStamp;
            if (f.partial) line += " (+unknown)";
            if (f.direct && f.directTarget < mod.functions.size()) line += " direct " + mod.functions[f.directTarget].name;
            if (!f.cause.empty()) line += " cause: " + f.cause;
            if (!f.escapeWhy.empty()) line += " escaped: " + f.escapeWhy;
            out += line + "\n";
        }
    }
    return out;
}

}  // namespace bronze::wpo
