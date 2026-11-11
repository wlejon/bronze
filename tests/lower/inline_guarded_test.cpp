// Guarded method inlining (`src/lower/inline_guarded.h`): own-slot closures
// fed from whole-program / feedback targets without duplicating fallback
// guards, and 2-way bimorphic prototype (`MethodIcIs`) and own-slot
// (`MethodSlotFn` + `ClosureIs`) guard chains.

#include <doctest/doctest.h>

#include <string>
#include <unordered_set>
#include <vector>

#include "il/verifier.h"
#include "lower/inline_guarded.h"
#include "lower_fixture.h"

using namespace bronze;

namespace {

std::optional<il::Module> lowerAot(std::string_view src, DiagnosticSink& diags, SourceBuffer& buf) {
    auto astMod = lower_test::parseOnly(src, diags, buf);
    if (diags.hasErrors() || !astMod) return std::nullopt;
    auto inferred = types::inferModule(*astMod, diags);
    if (diags.hasErrors() || !inferred) return std::nullopt;
    lower::AheadOfTimeOptions opts;
    return lower::lowerAheadOfTime(*astMod, diags, &*inferred, nullptr, opts);
}

const il::Function* findFnEndingIn(const il::Module& mod, std::string_view suffix) {
    for (const auto& fn : mod.functions) {
        if (fn.typedOrigin != il::Function::kNoTypedOrigin) continue;
        if (fn.name.size() >= suffix.size() &&
            fn.name.compare(fn.name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return &fn;
        }
    }
    return nullptr;
}

uint32_t findFnIndexEndingIn(const il::Module& mod, std::string_view suffix) {
    for (uint32_t i = 0; i < mod.functions.size(); ++i) {
        const auto& fn = mod.functions[i];
        if (fn.typedOrigin != il::Function::kNoTypedOrigin) continue;
        if (fn.name.size() >= suffix.size() &&
            fn.name.compare(fn.name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return i;
        }
    }
    return UINT32_MAX;
}

size_t countOp(const il::Function& fn, il::Op op) {
    size_t n = 0;
    for (const auto& b : fn.blocks) {
        for (const auto& inst : b.instructions) {
            if (inst.op == op) ++n;
        }
    }
    return n;
}

}  // namespace

TEST_CASE("inline guarded: own-callee closures from WPO inline without duplicate fallback guard") {
    // `route` hides the closure from `DirectMethodTable::closureGuesses`
    // (which only follows direct `create.func` and one parameter hop), so only
    // `wpo::predictDirectTargets` (`ownCallees`) discovers `cb`'s target.
    const std::string src =
        "function route(f: any) { const g = f; return g; }\n"
        "function makeCb(scale: any) {\n"
        "  return function onChange(v: any) { return v * scale + 1; };\n"
        "}\n"
        "class Binding {\n"
        "  cb: any;\n"
        "  constructor(s: any) { this.cb = route(makeCb(s)); }\n"
        "}\n"
        "function runLoop(list: any, n: any) {\n"
        "  let total: any = 0;\n"
        "  for (let i = 0; i < n; i++) total = total + list[i].cb(i);\n"
        "  return total;\n"
        "}\n"
        "function runOnce(b: any) { return b.cb(5); }\n"
        "const arr: any = [new Binding(2), new Binding(3)];\n"
        "runLoop(arr, 2);\n"
        "runOnce(arr[0]);\n";
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    auto mod = lowerAot(src, diags, buf);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(mod.has_value());

    const uint32_t onChangeIdx = findFnIndexEndingIn(*mod, "onChange");
    REQUIRE(onChangeIdx != UINT32_MAX);

    const il::Function* runLoop = findFnEndingIn(*mod, "runLoop");
    REQUIRE(runLoop != nullptr);
    bool inlinedOnChange = false;
    for (const auto& frame : runLoop->inlineFrames) {
        if (frame.callee == onChangeIdx) inlinedOnChange = true;
    }
    CHECK(inlinedOnChange);
    // Every `closure.is` in `runLoop` must come from guarded inlining (one per
    // `method.slot.fn`), never a second split of the cold fallback `method.call`
    // by `applyOwnCalleeCalls`.
    const size_t slotFns = countOp(*runLoop, il::Op::MethodSlotFn);
    const size_t closureTests = countOp(*runLoop, il::Op::ClosureIs);
    CHECK(slotFns >= 1);
    CHECK(closureTests == slotFns);

    // `runOnce` has no loops and is not called from one, so guarded inlining
    // leaves its call for `applyOwnCalleeCalls` to split into a direct call.
    const il::Function* runOnce = findFnEndingIn(*mod, "runOnce");
    REQUIRE(runOnce != nullptr);
    CHECK(runOnce->inlineFrames.empty());
    CHECK(countOp(*runOnce, il::Op::ClosureIs) == 1);

    DiagnosticSink verifyDiags;
    CHECK(il::verify(*mod, verifyDiags));
}

TEST_CASE("inline guarded: bimorphic prototype methods chain two MethodIcIs guards on one IC") {
    const std::string src =
        "class Lin { step(x: any) { return x; } }\n"
        "class Quat { step(x: any) { return x; } }\n"
        "function drive(cur: any, flag: any) {\n"
        "  while (flag) flag = cur.step(false);\n"
        "  return flag;\n"
        "}\n"
        "drive(new Lin(), true); drive(new Quat(), true);\n";
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    auto mod = lower_test::inferAndLower(src, diags, buf);
    REQUIRE(mod.has_value());

    const uint32_t linIdx = findFnIndexEndingIn(*mod, "Lin.step");
    const uint32_t quatIdx = findFnIndexEndingIn(*mod, "Quat.step");
    const uint32_t driveIdx = findFnIndexEndingIn(*mod, "drive");
    REQUIRE(linIdx != UINT32_MAX);
    REQUIRE(quatIdx != UINT32_MAX);
    REQUIRE(driveIdx != UINT32_MAX);

    const size_t stepKey = lower_test::keyIndex(*mod, "step");
    uint32_t stepIc = UINT32_MAX;
    for (const auto& b : mod->functions[driveIdx].blocks) {
        for (const auto& inst : b.instructions) {
            if (inst.op == il::Op::MethodCall && inst.keyIndex == stepKey) stepIc = inst.icIndex;
        }
    }
    REQUIRE(stepIc != UINT32_MAX);

    lower::ClosureGuesses polyMethods;
    polyMethods[stepIc] = {quatIdx, linIdx};
    std::vector<uint32_t> changed;
    CHECK(lower::inlineGuardedMethods(*mod, diags, &changed, nullptr, nullptr, &polyMethods));
    REQUIRE_FALSE(diags.hasErrors());
    lower::foldInlinedCallers(*mod, changed);

    const il::Function& drive = mod->functions[driveIdx];
    REQUIRE(drive.inlineFrames.size() == 2);
    CHECK(drive.inlineFrames[0].callee == quatIdx);
    CHECK(drive.inlineFrames[1].callee == linIdx);

    std::vector<uint32_t> guardCallees;
    il::BlockId firstMiss = il::kNoBlock, secondBlock = il::kNoBlock, secondMiss = il::kNoBlock;
    size_t fallbackCalls = 0;
    for (const auto& b : drive.blocks) {
        for (size_t i = 0; i < b.instructions.size(); ++i) {
            const auto& inst = b.instructions[i];
            if (inst.op == il::Op::MethodIcIs && inst.icIndex == stepIc) {
                guardCallees.push_back(inst.calleeIndex);
                REQUIRE(i + 1 < b.instructions.size());
                REQUIRE(b.instructions[i + 1].op == il::Op::Branch);
                if (guardCallees.size() == 1) {
                    firstMiss = b.instructions[i + 1].elseTarget.block;
                } else {
                    secondBlock = b.id;
                    secondMiss = b.instructions[i + 1].elseTarget.block;
                }
            }
            if (inst.op == il::Op::MethodCall && inst.icIndex == stepIc) {
                ++fallbackCalls;
                CHECK(b.id == secondMiss);
            }
        }
    }
    CHECK(guardCallees == std::vector<uint32_t>{quatIdx, linIdx});
    CHECK(firstMiss == secondBlock);
    CHECK(fallbackCalls == 1);

    DiagnosticSink verifyDiags;
    CHECK(il::verify(*mod, verifyDiags));
}

TEST_CASE("inline guarded: bimorphic own-slot closures reuse MethodSlotFn across two ClosureIs guards") {
    const std::string src =
        "function makeAdd(bias: any) { return function addFn(x: any) { return bias ? x : false; }; }\n"
        "function makeMul(scale: any) { return function mulFn(x: any) { return scale ? x : false; }; }\n"
        "function applyAll(cur: any, flag: any) {\n"
        "  while (flag) flag = cur.mix(false);\n"
        "  return flag;\n"
        "}\n"
        "applyAll({ mix: makeAdd(true) }, true); applyAll({ mix: makeMul(true) }, true);\n";
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    auto mod = lower_test::inferAndLower(src, diags, buf);
    REQUIRE(mod.has_value());

    const uint32_t addIdx = findFnIndexEndingIn(*mod, "addFn");
    const uint32_t mulIdx = findFnIndexEndingIn(*mod, "mulFn");
    const uint32_t callerIdx = findFnIndexEndingIn(*mod, "applyAll");
    REQUIRE(addIdx != UINT32_MAX);
    REQUIRE(mulIdx != UINT32_MAX);
    REQUIRE(callerIdx != UINT32_MAX);

    const size_t mixKey = lower_test::keyIndex(*mod, "mix");
    uint32_t mixIc = UINT32_MAX;
    for (const auto& b : mod->functions[callerIdx].blocks) {
        for (const auto& inst : b.instructions) {
            if (inst.op == il::Op::MethodCall && inst.keyIndex == mixKey) mixIc = inst.icIndex;
        }
    }
    REQUIRE(mixIc != UINT32_MAX);

    lower::ClosureGuesses polyOwn;
    polyOwn[mixIc] = {mulIdx, addIdx};
    std::vector<uint32_t> changed;
    std::unordered_set<uint32_t> inlinedOwnIcs;
    CHECK(lower::inlineGuardedMethods(*mod, diags, &changed, nullptr, nullptr, nullptr, &polyOwn, &inlinedOwnIcs));
    REQUIRE_FALSE(diags.hasErrors());
    CHECK(inlinedOwnIcs.count(mixIc) == 1);

    const il::Function& caller = mod->functions[callerIdx];
    REQUIRE(caller.inlineFrames.size() == 2);
    CHECK(caller.inlineFrames[0].callee == mulIdx);
    CHECK(caller.inlineFrames[1].callee == addIdx);

    // One `method.slot.fn` dominates both `closure.is` tests and both
    // `closure.env` reads; one fallback `method.call` remains on the final miss.
    il::ValueId slotVal = il::kNoValue;
    size_t slotFnCount = 0;
    std::vector<uint32_t> testedCallees;
    size_t envReads = 0;
    size_t fallbackCalls = 0;
    for (const auto& b : caller.blocks) {
        for (const auto& inst : b.instructions) {
            if (inst.op == il::Op::MethodSlotFn && inst.icIndex == mixIc) {
                ++slotFnCount;
                slotVal = inst.result;
            }
            if (inst.op == il::Op::ClosureIs) {
                REQUIRE(!inst.operands.empty());
                CHECK(inst.operands[0] == slotVal);
                testedCallees.push_back(inst.calleeIndex);
            }
            if (inst.op == il::Op::ClosureEnv) {
                REQUIRE(!inst.operands.empty());
                CHECK(inst.operands[0] == slotVal);
                ++envReads;
            }
            if (inst.op == il::Op::MethodCall && inst.icIndex == mixIc) ++fallbackCalls;
        }
    }
    CHECK(slotFnCount == 1);
    CHECK(testedCallees == std::vector<uint32_t>{mulIdx, addIdx});
    CHECK(envReads == 2);
    CHECK(fallbackCalls == 1);

    DiagnosticSink verifyDiags;
    CHECK(il::verify(*mod, verifyDiags));
}

TEST_CASE("inline guarded: bimorphic site refuses partial inline when an arm is ineligible") {
    const std::string src =
        "class Fast { step(x: any) { return x + 1; } }\n"
        "class Variadic { step(...xs: any[]) { return xs.length; } }\n"
        "function drive(arr: any, n: any) {\n"
        "  let s: any = 0;\n"
        "  for (let i = 0; i < n; i++) s = s + arr[i].step(i);\n"
        "  return s;\n"
        "}\n"
        "drive([new Fast(), new Variadic()], 2);\n";
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    auto mod = lower_test::inferAndLower(src, diags, buf);
    REQUIRE(mod.has_value());

    const uint32_t fastIdx = findFnIndexEndingIn(*mod, "Fast.step");
    const uint32_t varIdx = findFnIndexEndingIn(*mod, "Variadic.step");
    const uint32_t driveIdx = findFnIndexEndingIn(*mod, "drive");
    REQUIRE(fastIdx != UINT32_MAX);
    REQUIRE(varIdx != UINT32_MAX);
    REQUIRE(driveIdx != UINT32_MAX);

    const size_t stepKey = lower_test::keyIndex(*mod, "step");
    uint32_t stepIc = UINT32_MAX;
    for (const auto& b : mod->functions[driveIdx].blocks) {
        for (const auto& inst : b.instructions) {
            if (inst.op == il::Op::MethodCall && inst.keyIndex == stepKey) stepIc = inst.icIndex;
        }
    }
    REQUIRE(stepIc != UINT32_MAX);

    lower::ClosureGuesses polyMethods;
    polyMethods[stepIc] = {fastIdx, varIdx};
    CHECK_FALSE(lower::inlineGuardedMethods(*mod, diags, nullptr, nullptr, nullptr, &polyMethods));
    CHECK(countOp(mod->functions[driveIdx], il::Op::MethodIcIs) == 0);
}
