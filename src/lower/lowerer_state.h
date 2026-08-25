#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast/ast.h"
#include "il/il.h"

namespace bronze::lower {

struct LowererValue {
    il::ValueId id = il::kNoValue;
    il::Type type = il::Type::Dynamic;
};

struct VarBinding {
    std::string name;
    il::Type type = il::Type::Dynamic;
    bool isConst = false;
    bool isLet = false;
    bool isVar = false;
    bool isInitialized = true;
    uint32_t declOrder = 0;
    size_t scopeDepth = 0;
    il::ValueId valueId = il::kNoValue;
    bool inEnv = false;
    size_t envScopeIndex = 0;
    uint32_t envSlot = 0;
    size_t shadowedBinding = SIZE_MAX;
    bool isTdzHoisted = false;
};

enum class JumpKind { Loop, Switch, LabeledBlock };

struct JumpTarget {
    JumpKind kind = JumpKind::Loop;
    std::string label;
    il::BlockId headerBlock = il::kNoBlock;
    il::BlockId updateBlock = il::kNoBlock;
    il::BlockId exitBlock = il::kNoBlock;
    std::vector<std::string> vars;
    size_t cleanupDepthAtEntry = 0;
    size_t cleanupDepthInBody = 0;
    il::ValueId perIterationEnv = il::kNoValue;
};

enum class CleanupKind {
    Finally,
    IteratorClose,
    AsyncIteratorClose,
};

struct CleanupFrame {
    CleanupKind kind = CleanupKind::Finally;
    const ast::TryStmt* stmt = nullptr;
    il::ValueId iterRecord = il::kNoValue;
    uint32_t iterFrameSlot = UINT32_MAX;
    size_t jumpDepth = 0;
    il::BlockId outerHandler = il::kNoBlock;
};

struct EnvScopeInfo {
    std::unordered_map<std::string, uint32_t> slotOf;
    std::vector<std::string> slotNames;
    std::vector<bool> slotIsLexical;
    std::vector<bool> slotIsImmutable;
    // Which slots hold a Number at every read (lower_scope.cpp
    // `planEnvSlotNumberTypes`). The record itself is unchanged — a canonical
    // double IS a Value by NaN-box construction and the collector already walks
    // past one — so what this marks is that the READ may stop testing.
    std::vector<bool> slotIsF64;
    il::ValueId envValue = il::kNoValue;
    uint32_t childSlot = UINT32_MAX;
};

struct VarState {
    il::ValueId valueId = il::kNoValue;
    il::Type type = il::Type::Dynamic;
};
using VarStateMap = std::unordered_map<std::string, VarState>;

struct ExprJoin {
    std::vector<std::string> vars;
    std::unordered_map<std::string, il::ValueId> paramId;
    std::unordered_map<std::string, il::Type> paramType;
};

struct StaticSlotSite {
    uint32_t slot = 0;
    uint32_t cellIndex = 0;
    uint32_t familyLo = il::Instruction::kNoFamily;
    uint32_t familySpan = 0;
};

struct GeneratorContext {
    size_t frameScope = SIZE_MAX;
    il::ValueId frameEnv = il::kNoValue;
    il::ValueId modeParam = il::kNoValue;
    il::ValueId sentParam = il::kNoValue;
    uint32_t stateSlot = 0;
    uint32_t iterSlot = UINT32_MAX;
    bool isAsync = false;
    bool isAsyncGenerator = false;
    uint32_t machineSlot = UINT32_MAX;
    std::vector<uint32_t> loopIterSlots;
    uint32_t activeIterLoops = 0;

    static constexpr double kModeNext = 0.0;
    static constexpr double kModeReturn = 1.0;
    static constexpr double kModeThrow = 2.0;
    std::vector<il::BlockId> resumeBlocks;
};

}  // namespace bronze::lower
