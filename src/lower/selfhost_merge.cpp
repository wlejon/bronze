// The self-hosted library compiled into an ahead-of-time program (lower.h,
// SelfHostLibrary): its builtins that the program's call sites can reach are
// appended to the module as program functions, and each such site gets a
// direct edge into them.
//
// A site `r = method.call recv, "map", <site>, f, t` in block B becomes
//
//     B:  ...head...
//         g = is.plain_array recv
//         br g, D, S
//     D:  r1 = call @selfhost.map(recv, f, t)      ; padded with undefined,
//         jump C(r1)                               ;   extra arguments dropped
//     S:  r2 = method.call recv, "map", <site>, f, t
//         jump C(r2)
//     C(r): ...the rest of B...
//
// THE INVARIANT that makes the copy unobservable. `is.plain_array` holds only
// for an Array at the bare array shape, and such an array's [[Get]] of
// "map" answers the intrinsic Array.prototype.map: nothing can shadow it (an
// own property, a subclass's or a replaced prototype, an integrity level all
// move the array off that shape) and nothing can replace it (Array.prototype refuses
// every write and define, rt_prop_write.cpp). The intrinsic is the prebuilt
// object's function compiled from the same library text by the same lowering
// (src/selfhost/selfhost_library.h), so D runs the very algorithm S would have
// entered, with the same receiver and arguments, and its frame is reported
// the same way (the builtin's descriptor: BUILTIN under its V8 name, HIDDEN
// for a helper). `Array.prototype.map` as a value is still the one prebuilt
// function object; the program's copy is never a function object. Where the
// guard misses (a Map's forEach, an array-like, an array with an expando, a
// subclass instance), S is the ordinary call it always was.
//
// A builtin with a rest parameter names a DIRECT ENTRY (builtins.js
// `reduceCounted`): D calls it with the receiver, the declared parameters and
// the argument count the site passes, which is what the rest array's length
// told the registered function.
//
// Only what a site can reach is merged: a program with no such site gets
// nothing, not even a source file.
//
// A C++ builtin with a FAST ENTRY (runtime/native_fast.h) gets the same split
// at a site passing it at most the entry's arguments: D calls the entry with
// the receiver and the arguments in registers (padded with undefined, which
// each entry's builtin reads as the absent argument), which answers the
// common case and hands the rest to the builtin. The guard is the same
// invariant for the entry's receiver kind: on a plain array an Array.prototype
// method is the intrinsic, and so is a String.prototype method on a string
// primitive (`typeof.is string`) and a Number.prototype one on a number
// (`typeof.is number`), whose [[Get]] reaches the frozen prototype with
// nothing in between (runtime/lockdown.h). A method name with entries for
// several kinds (`indexOf`) is split once per kind, each test on the
// previous one's other edge.

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "il/fast_entries.h"
#include "lower/lowerer.h"

namespace bronze::lower {

namespace {

constexpr const char* kPrefix = "selfhost.";
constexpr const char* kArrayPrototype = "Array.prototype.";

// Whether the program can carry `inst` as written, its module-global indices
// remapped: key, IC site (the four ops lowering gives one), and function. An
// instruction naming any other module table (a static cell, a template site,
// a native import, the module scope's record) keeps its function out.
bool mergeable(const il::Instruction& inst) {
    switch (inst.op) {
        case il::Op::ModuleEnvGet:
        case il::Op::ModuleEnvSet:
        case il::Op::TemplateCached:
        case il::Op::TemplateObject:
        case il::Op::GlobalGet:
        case il::Op::ResolveName:
        case il::Op::ImportMeta:
        case il::Op::DynamicImport:
        case il::Op::PinGuard:
        case il::Op::FeedbackRecord:
        case il::Op::CoroStart:
        case il::Op::CreateFunction:
        case il::Op::EnvCreate:
        case il::Op::EnvGet:
        case il::Op::EnvSet:
        case il::Op::EnvGetTdz:
        case il::Op::EnvInitTdz:
            return false;
        default:
            break;
    }
    return inst.directTarget == il::Instruction::kNoDirectTarget;
}

bool namesFunction(il::Op op) { return op == il::Op::Call || op == il::Op::FunctionRef; }

bool carriesIcSite(il::Op op) {
    return op == il::Op::PropGet || op == il::Op::PropSet || op == il::Op::MethodCall ||
           op == il::Op::MethodCallSpread || op == il::Op::ApplyArgs;
}

using il::FastEntry;
using il::FastReceiver;
using il::kFastEntries;

struct Site {
    uint32_t function = 0;
    il::BlockId block = 0;
    size_t index = 0;
    const SelfHostLibrary::Builtin* builtin = nullptr;
    // Else the fast entries' rows for the site's method, in split order.
    std::vector<const FastEntry*> fast;
};

// The receiver test of a kind: its op and immediate.
std::pair<il::Op, int32_t> receiverGuard(FastReceiver r) {
    switch (r) {
        case FastReceiver::PlainArray:
            return {il::Op::IsPlainArray, 0};
        case FastReceiver::String:
            return {il::Op::TypeOfIs, il::kTypeOfString};
        case FastReceiver::Number:
            return {il::Op::TypeOfIs, il::kTypeOfNumber};
        case FastReceiver::RegExp:
            return {il::Op::IsCollection, BRONZE_ABI_COLLECTION_REGEXP};
    }
    return {il::Op::IsPlainArray, 0};
}

}  // namespace

bool Lowerer::mergeSelfHostLibrary() {
    const SelfHostLibrary& lib = *selfHostLibrary_;
    il::Module& mod = ilModule_;
    const il::Module& src = lib.module;

    // Which library functions a builtin's direct entry reaches, and whether
    // all of them can be carried: a builtin that cannot is left to its call.
    std::vector<std::vector<uint32_t>> reach(src.functions.size());
    const auto closure = [&](uint32_t root) {
        std::vector<uint32_t> out{root};
        std::unordered_set<uint32_t> seen{root};
        for (size_t i = 0; i < out.size(); ++i) {
            const il::Function& fn = src.functions[out[i]];
            for (const il::Block& b : fn.blocks) {
                for (const il::Instruction& inst : b.instructions) {
                    if (!namesFunction(inst.op) || inst.calleeIndex >= src.functions.size()) continue;
                    if (src.functions[inst.calleeIndex].isExternal) continue;
                    if (seen.insert(inst.calleeIndex).second) out.push_back(inst.calleeIndex);
                }
            }
        }
        return out;
    };
    const auto carried = [&](uint32_t f) {
        const il::Function& fn = src.functions[f];
        if (fn.needsEnv || fn.needsArguments || fn.isGenerator || fn.isCoroutineBody() || fn.isEntryPoint) {
            return false;
        }
        for (const il::Block& b : fn.blocks) {
            for (const il::Instruction& inst : b.instructions) {
                if (!mergeable(inst)) return false;
                if (inst.op == il::Op::Call && inst.calleeIndex < src.functions.size() &&
                    src.functions[inst.calleeIndex].isExternal &&
                    src.functions[inst.calleeIndex].name.rfind("__bronze_native_", 0) == 0) {
                    return false;
                }
            }
        }
        return true;
    };
    std::unordered_map<std::string, const SelfHostLibrary::Builtin*> byMethod;
    for (const SelfHostLibrary::Builtin& b : lib.builtins) {
        if (b.key.rfind(kArrayPrototype, 0) != 0 || b.direct >= src.functions.size()) continue;
        std::vector<uint32_t> fns = closure(b.direct);
        if (!std::all_of(fns.begin(), fns.end(), carried)) continue;
        const il::Function& entry = src.functions[b.direct];
        // The site hands the entry Dynamic values (the receiver and its
        // arguments); an entry that types one of those parameters cannot
        // take them. A counted entry's last parameter is the count.
        if (entry.needsThis == b.counted || entry.hasRestParam || entry.needsEnv || entry.needsArguments ||
            entry.params.size() < entry.firstSourceParam() + (b.counted ? 2 : 0)) {
            continue;
        }
        bool dynamicParams = true;
        const size_t last = entry.params.size() - (b.counted ? 1 : 0);
        for (size_t p = entry.firstSourceParam(); p < last; ++p) {
            dynamicParams = dynamicParams && entry.params[p].type == il::Type::Dynamic;
        }
        if (b.counted && entry.params.back().type != il::Type::Dynamic && entry.params.back().type != il::Type::F64) {
            dynamicParams = false;
        }
        if (!dynamicParams) continue;
        reach[b.direct] = std::move(fns);
        byMethod.emplace(b.method, &b);
    }

    // The sites: a method call of one of those keys, its operands boxed.
    std::vector<Site> sites;
    std::vector<uint8_t> used(src.functions.size(), 0);
    for (size_t f = 0; f < mod.functions.size(); ++f) {
        const il::Function& fn = mod.functions[f];
        if (fn.isExternal) continue;
        std::vector<il::Type> types(fn.valueCount, il::Type::Void);
        for (size_t p = 0; p < fn.params.size() && p < types.size(); ++p) types[p] = fn.params[p].type;
        for (const il::Block& b : fn.blocks) {
            for (const il::BlockParam& p : b.params) {
                if (p.id < types.size()) types[p.id] = p.type;
            }
            for (const il::Instruction& inst : b.instructions) {
                if (inst.result != il::kNoValue && inst.result < types.size()) types[inst.result] = inst.type;
            }
        }
        for (size_t b = 0; b < fn.blocks.size(); ++b) {
            for (size_t i = 0; i < fn.blocks[b].instructions.size(); ++i) {
                const il::Instruction& inst = fn.blocks[b].instructions[i];
                if (inst.op != il::Op::MethodCall || inst.operands.empty() || inst.keyIndex >= mod.keyConstants.size()) {
                    continue;
                }
                const std::string& method = mod.keyConstants[inst.keyIndex];
                const auto it = byMethod.find(method);
                std::vector<const FastEntry*> fast;
                for (const FastEntry& e : kFastEntries) {
                    if (method == e.method && inst.operands.size() <= 1 + e.argc) fast.push_back(&e);
                }
                if (it == byMethod.end() && fast.empty()) continue;
                if (inst.result != il::kNoValue && inst.type != il::Type::Dynamic) continue;
                bool boxed = true;
                for (il::ValueId v : inst.operands) boxed = boxed && v < types.size() && types[v] == il::Type::Dynamic;
                if (!boxed) continue;
                if (it == byMethod.end()) {
                    sites.push_back({static_cast<uint32_t>(f), static_cast<il::BlockId>(b), i, nullptr, std::move(fast)});
                    continue;
                }
                sites.push_back({static_cast<uint32_t>(f), static_cast<il::BlockId>(b), i, it->second, {}});
                for (uint32_t g : reach[it->second->direct]) used[g] = 1;
            }
        }
    }
    if (sites.empty()) return true;
    const bool anyUsed = std::find(used.begin(), used.end(), 1) != used.end();

    // The library's file joins the program's, after them (its text only
    // where the program keeps its own, for the harvest identity).
    const bool positions = !mod.lineTables.empty();
    const uint16_t file = static_cast<uint16_t>(mod.lineTables.size());
    if (positions && anyUsed) {
        if (mod.sourceTexts.size() == mod.lineTables.size()) mod.sourceTexts.push_back(lib.sourceText);
        mod.sourceFiles.resize(mod.lineTables.size());
        mod.sourceFiles.push_back(lib.sourceName);
        mod.lineTables.emplace_back(lib.sourceText);
    }

    // Indices: every used function appended in library order, externals
    // shared with the program's by symbol.
    std::vector<uint32_t> fnMap(src.functions.size(), UINT32_MAX);
    for (size_t g = 0; g < src.functions.size(); ++g) {
        if (!used[g]) continue;
        fnMap[g] = static_cast<uint32_t>(mod.functions.size());
        mod.functions.push_back(il::Function{});
    }
    const auto remapFn = [&](uint32_t g) {
        if (fnMap[g] != UINT32_MAX) return fnMap[g];
        const il::Function& ext = src.functions[g];
        std::vector<il::Type> paramTypes;
        for (const il::Param& p : ext.params) paramTypes.push_back(p.type);
        return fnMap[g] = registerExternalFunction(ext.name, ext.returnType, paramTypes);
    };
    std::vector<uint32_t> keyMap(src.keyConstants.size(), UINT32_MAX);
    const auto remapKey = [&](uint32_t k) {
        if (k >= keyMap.size()) return k;
        if (keyMap[k] == UINT32_MAX) {
            keyMap[k] = getKeyConstantIndex(src.keyConstants[k]);
            if (keyMap[k] >= mod.keyConstants.size()) mod.keyConstants.resize(keyMap[k] + 1);
            mod.keyConstants[keyMap[k]] = src.keyConstants[k];
        }
        return keyMap[k];
    };
    const uint32_t icBase = mod.icSiteCount;
    mod.icSiteCount += src.icSiteCount;
    for (size_t g = 0; g < src.functions.size(); ++g) {
        if (!used[g]) continue;
        il::Function fn = src.functions[g];
        fn.name = kPrefix + fn.name;
        fn.isExported = false;
        // Never a function object (that is the prebuilt one), so none of the
        // object's flags: a NATIVE one is what a direct call may not enter.
        fn.fnFlags = 0;
        // Never a function object of its own, so no source text to give
        // toString: the spans stay, for the frames' lines.
        fn.sourceFile = file;
        fn.sourceBegin = fn.sourceEnd = 0;
        if (fn.nameKeyIndex != 0xFFFFFFFFu) fn.nameKeyIndex = remapKey(fn.nameKeyIndex);
        for (il::Param& p : fn.params) {
            if (p.pinned) p.pinKeyIndex = remapKey(p.pinKeyIndex);
        }
        for (il::Block& b : fn.blocks) {
            for (il::Instruction& inst : b.instructions) {
                inst.keyIndex = remapKey(inst.keyIndex);
                if (carriesIcSite(inst.op)) inst.icIndex += icBase;
                if (namesFunction(inst.op)) inst.calleeIndex = remapFn(inst.calleeIndex);
                if (positions) {
                    inst.span.file = file;
                } else {
                    inst.span = Span{};
                }
            }
        }
        mod.functions[fnMap[g]] = std::move(fn);
    }

    // The sites, last to first within a block so the earlier indices stand.
    std::sort(sites.begin(), sites.end(), [](const Site& a, const Site& b) {
        if (a.function != b.function) return a.function < b.function;
        if (a.block != b.block) return a.block < b.block;
        return a.index > b.index;
    });
    for (const Site& site : sites) {
        if (!site.fast.empty()) {
            // Each further kind's test sits on the previous one's other edge,
            // where the original call now is, alone at the head of its block.
            il::BlockId block = site.block;
            size_t index = site.index;
            for (const FastEntry* fast : site.fast) {
                const std::vector<il::Type> params(1 + fast->argc, il::Type::Dynamic);
                const uint32_t target = registerExternalFunction(fast->symbol, il::Type::Dynamic, params);
                const auto [op, imm] = receiverGuard(fast->receiver);
                block = splitOnReceiver(mod.functions[site.function], block, index, op, imm, target, fast->argc,
                                        false, false);
                index = 0;
            }
            continue;
        }
        const uint32_t target = fnMap[site.builtin->direct];
        const il::Function& entry = mod.functions[target];
        splitOnReceiver(mod.functions[site.function], site.block, site.index, il::Op::IsPlainArray, 0, target,
                        entry.params.size() - entry.firstSourceParam() - (site.builtin->counted ? 2 : 0),
                        site.builtin->counted, entry.returnType == il::Type::Void,
                        entry.params.back().type == il::Type::Dynamic);
    }
    return true;
}

// The split of one site (the file's comment): on `guardOp` of the receiver,
// `target` is entered with the receiver, `forwarded` of the call's arguments
// (padded with undefined, extra ones dropped) and, `counted`, the argument
// count (boxed when `boxedCount`). Answers the block the original call moved
// to, where it is the first instruction.
il::BlockId Lowerer::splitOnReceiver(il::Function& fn, il::BlockId b, size_t index, il::Op guardOp,
                                     int32_t guardImm, uint32_t target, size_t forwarded, bool counted,
                                     bool voidEntry, bool boxedCount) {
    const il::Instruction call = fn.blocks[b].instructions[index];
    const auto newBlock = [&]() -> il::BlockId {
        il::Block blk;
        blk.id = static_cast<il::BlockId>(fn.blocks.size());
        blk.handler = fn.blocks[b].handler;
        fn.blocks.push_back(std::move(blk));
        return fn.blocks.back().id;
    };
    const auto at = [&](il::Op op, il::Type type) {
        il::Instruction inst;
        inst.op = op;
        inst.type = type;
        inst.span = call.span;
        return inst;
    };
    const il::BlockId direct = newBlock();
    const il::BlockId slow = newBlock();
    const il::BlockId tail = newBlock();
    {
        auto& insts = fn.blocks[b].instructions;
        std::vector<il::Instruction> rest(insts.begin() + static_cast<std::ptrdiff_t>(index) + 1, insts.end());
        insts.resize(index);
        fn.blocks[tail].instructions = std::move(rest);
        if (call.result != il::kNoValue) fn.blocks[tail].params.push_back({call.result, il::Type::Dynamic});
    }
    il::Instruction guard = at(guardOp, il::Type::Bool);
    guard.immI32 = guardImm;
    guard.result = fn.valueCount++;
    guard.operands = {call.operands[0]};
    il::Instruction br = at(il::Op::Branch, il::Type::Void);
    br.operands = {guard.result};
    br.target.block = direct;
    br.elseTarget.block = slow;
    fn.blocks[b].instructions.push_back(std::move(guard));
    fn.blocks[b].instructions.push_back(std::move(br));

    // D: the entry's parameters, from the call's operands.
    il::Block& d = fn.blocks[direct];
    std::vector<il::ValueId> operands{call.operands[0]};
    il::ValueId undef = il::kNoValue;
    for (size_t a = 0; a < forwarded; ++a) {
        if (1 + a < call.operands.size()) {
            operands.push_back(call.operands[1 + a]);
            continue;
        }
        if (undef == il::kNoValue) {
            il::Instruction u = at(il::Op::ConstUndefined, il::Type::Dynamic);
            u.result = undef = fn.valueCount++;
            d.instructions.push_back(std::move(u));
        }
        operands.push_back(undef);
    }
    if (counted) {
        il::Instruction count = at(il::Op::ConstF64, il::Type::F64);
        count.immF64 = static_cast<double>(call.operands.size() - 1);
        count.result = fn.valueCount++;
        il::ValueId v = count.result;
        d.instructions.push_back(std::move(count));
        if (boxedCount) {
            il::Instruction box = at(il::Op::Box, il::Type::Dynamic);
            box.boxType = il::Type::F64;
            box.operands = {v};
            box.result = v = fn.valueCount++;
            d.instructions.push_back(std::move(box));
        }
        operands.push_back(v);
    }
    il::Instruction enter = at(il::Op::Call, il::Type::Dynamic);
    enter.calleeIndex = target;
    enter.operands = std::move(operands);
    il::Instruction toTail = at(il::Op::Jump, il::Type::Void);
    toTail.target.block = tail;
    if (voidEntry) enter.type = il::Type::Void;
    il::ValueId answer = il::kNoValue;
    if (call.result != il::kNoValue && !voidEntry) answer = enter.result = fn.valueCount++;
    d.instructions.push_back(std::move(enter));
    if (call.result != il::kNoValue && voidEntry) {
        il::Instruction u = at(il::Op::ConstUndefined, il::Type::Dynamic);
        u.result = answer = fn.valueCount++;
        d.instructions.push_back(std::move(u));
    }
    if (answer != il::kNoValue) toTail.target.args.push_back(answer);
    d.instructions.push_back(std::move(toTail));

    // S: the original call, its result renamed.
    il::Instruction again = call;
    il::Instruction join = at(il::Op::Jump, il::Type::Void);
    join.target.block = tail;
    if (call.result != il::kNoValue) {
        again.result = fn.valueCount++;
        join.target.args.push_back(again.result);
    }
    fn.blocks[slow].instructions.push_back(std::move(again));
    fn.blocks[slow].instructions.push_back(std::move(join));
    return slow;
}

}  // namespace bronze::lower
