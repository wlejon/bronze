#include "il/serialize.h"

#include <cstring>
#include <deque>
#include <type_traits>

namespace bronze::il {

namespace {

// ---------------------------------------------------------------------------
// Layout guards. Each mirror lists its struct's data members in declaration
// order; a member added to the real struct without being added here (and to
// the encoder below) changes its size and stops the build. A new member small
// enough to hide in padding would slip past, which the round-trip test in
// tests/il/il_serialize_test.cpp is the second net for.
// ---------------------------------------------------------------------------

struct InstructionMirror {
    Op op;
    Type type;
    ValueId result;
    std::vector<ValueId> operands;
    double immF64;
    int32_t immI32;
    uint32_t calleeIndex;
    Type boxType;
    uint32_t keyIndex;
    uint32_t icIndex;
    uint32_t slowSite;
    uint32_t directTarget;
    uint32_t envDepth;
    uint32_t envIndex;
    bool envImmutable;
    uint32_t callEnvHops;
    uint32_t resumeAfter;
    uint32_t inlineScope;
    Span span;
    BlockTarget target;
    BlockTarget elseTarget;
};
static_assert(sizeof(Instruction) == sizeof(InstructionMirror),
              "il::Instruction changed: update il/serialize.cpp and bump kFormatVersion");

struct BlockMirror {
    BlockId id;
    std::vector<BlockParam> params;
    std::vector<Instruction> instructions;
    bool cold;
    BlockId handler;
};
static_assert(sizeof(Block) == sizeof(BlockMirror),
              "il::Block changed: update il/serialize.cpp and bump kFormatVersion");

struct ParamMirror {
    std::string name;
    Type type;
    bool pinned;
    uint32_t pinKeyIndex;
};
static_assert(sizeof(Param) == sizeof(ParamMirror),
              "il::Param changed: update il/serialize.cpp and bump kFormatVersion");

struct FunctionMirror {
    std::string name;
    std::vector<Param> params;
    Type returnType;
    bool returnPinned;
    uint32_t returnPinKeyIndex;
    bool isExported;
    bool isExternal;
    bool isEntryPoint;
    bool needsEnv;
    bool needsThis;
    bool needsArguments;
    bool argumentsIsList;
    bool argumentsIsCount;
    bool isStrict;
    bool isGenerator;
    int8_t coroKind;
    uint32_t fnFlags;
    bool hasRestParam;
    uint32_t requiredArgs;
    uint32_t nameKeyIndex;
    uint16_t sourceFile;
    uint32_t sourceBegin;
    uint32_t sourceEnd;
    std::string displayName;
    uint32_t descFlags;
    uint32_t speculatedFrom;
    uint32_t fallback;
    uint32_t entrySlowSite;
    std::vector<Function::InlineScope> inlineScopes;
    std::vector<Block> blocks;
    uint32_t valueCount;
};
static_assert(sizeof(Function) == sizeof(FunctionMirror),
              "il::Function changed: update il/serialize.cpp and bump kFormatVersion");

struct NativeImportMirror {
    std::string name;
    std::string signature;
    uint32_t functionIndex;
    uint32_t bufferReturnKind;
};
static_assert(sizeof(Module::NativeImport) == sizeof(NativeImportMirror),
              "il::Module::NativeImport changed: update il/serialize.cpp and bump kFormatVersion");

struct ModuleMirror {
    std::string name;
    std::vector<std::string> keyConstants;
    uint32_t icSiteCount;
    uint32_t templateSiteCount;
    uint32_t slowSiteCount;
    std::vector<uint32_t> pretenureSites;
    bool mathPinned;
    bool numberPinned;
    std::vector<Module::CensusSiteEntry> censusSites;
    std::string censusOutPath;
    uint64_t icLayoutDigest;
    std::string harvestOutPath;
    std::vector<Module::NativeImport> nativeImports;
    std::vector<Module::ShapeCell> shapeCells;
    std::vector<uint32_t> numberSlotKeys;
    std::vector<Module::ObjectLiteral> objectLiterals;
    std::vector<Module::ProtoCell> protoCells;
    std::vector<Module::MemberCell> memberCells;
    std::deque<Function> functions;
    std::vector<std::string> sourceTexts;
    std::vector<std::string> sourceFiles;
    std::vector<LineTable> lineTables;
};
struct ShapeCellMirror {
    std::string key;
};
static_assert(sizeof(Module::ShapeCell) == sizeof(ShapeCellMirror),
              "il::Module::ShapeCell changed: update il/serialize.cpp and bump kFormatVersion");

struct ObjectLiteralMirror {
    std::vector<uint32_t> keys;
    uint32_t inlineSlots;
};
static_assert(sizeof(Module::ObjectLiteral) == sizeof(ObjectLiteralMirror),
              "il::Module::ObjectLiteral changed: update il/serialize.cpp and bump kFormatVersion");
struct ProtoCellMirror {
    uint32_t fromCell;
    uint32_t keyIndex;
    uint32_t toCell;
    int32_t slot;
};
static_assert(sizeof(Module::ProtoCell) == sizeof(ProtoCellMirror),
              "il::Module::ProtoCell changed: update il/serialize.cpp and bump kFormatVersion");
struct MemberCellMirror {
    uint8_t kind;
    uint32_t keyIndex;
    uint32_t callee;
    uint32_t group;
    uint32_t groupSize;
};
static_assert(sizeof(Module::MemberCell) == sizeof(MemberCellMirror),
              "il::Module::MemberCell changed: update il/serialize.cpp and bump kFormatVersion");

static_assert(sizeof(Module) == sizeof(ModuleMirror),
              "il::Module changed: update il/serialize.cpp and bump kFormatVersion");

// ---------------------------------------------------------------------------
// Encoding: LEB128 varints for every integer, raw little-endian bits for a
// double, a varint length before every string and vector.
// ---------------------------------------------------------------------------

class Writer {
public:
    void u8(uint8_t v) { out_.push_back(static_cast<char>(v)); }
    void var(uint64_t v) {
        while (v >= 0x80) {
            out_.push_back(static_cast<char>((v & 0x7F) | 0x80));
            v >>= 7;
        }
        out_.push_back(static_cast<char>(v));
    }
    void svar(int64_t v) { var((static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63)); }
    void f64(double d) {
        char raw[8];
        std::memcpy(raw, &d, 8);
        out_.append(raw, 8);
    }
    void str(std::string_view s) {
        var(s.size());
        out_.append(s.data(), s.size());
    }
    std::string take() { return std::move(out_); }

private:
    std::string out_;
};

class Reader {
public:
    explicit Reader(std::string_view bytes) : p_(bytes.data()), end_(bytes.data() + bytes.size()) {}

    bool ok() const { return ok_; }
    bool atEnd() const { return p_ == end_; }
    const std::string& error() const { return err_; }
    void fail(const char* what) {
        if (ok_) err_ = what;
        ok_ = false;
        p_ = end_;
    }

    uint8_t u8() {
        if (p_ >= end_) {
            fail("truncated");
            return 0;
        }
        return static_cast<uint8_t>(*p_++);
    }
    uint64_t var() {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (p_ >= end_) {
                fail("truncated varint");
                return 0;
            }
            const uint8_t b = static_cast<uint8_t>(*p_++);
            v |= static_cast<uint64_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) return v;
        }
        fail("overlong varint");
        return 0;
    }
    uint32_t u32() {
        const uint64_t v = var();
        if (v > UINT32_MAX) {
            fail("u32 out of range");
            return 0;
        }
        return static_cast<uint32_t>(v);
    }
    int32_t i32() {
        const uint64_t z = var();
        const int64_t v = static_cast<int64_t>(z >> 1) ^ -static_cast<int64_t>(z & 1);
        if (v < INT32_MIN || v > INT32_MAX) {
            fail("i32 out of range");
            return 0;
        }
        return static_cast<int32_t>(v);
    }
    double f64() {
        if (end_ - p_ < 8) {
            fail("truncated f64");
            return 0;
        }
        double d;
        std::memcpy(&d, p_, 8);
        p_ += 8;
        return d;
    }
    std::string str() {
        const uint64_t n = var();
        if (n > static_cast<uint64_t>(end_ - p_)) {
            fail("string runs past the end");
            return {};
        }
        std::string s(p_, static_cast<size_t>(n));
        p_ += n;
        return s;
    }
    // A count of elements that each take at least one byte: bounded by what
    // is left, so a corrupt count cannot ask for a huge allocation.
    size_t count() {
        const uint64_t n = var();
        if (n > static_cast<uint64_t>(end_ - p_)) {
            fail("count runs past the end");
            return 0;
        }
        return static_cast<size_t>(n);
    }
    Type type() {
        const uint8_t t = u8();
        if (t > static_cast<uint8_t>(Type::Ptr)) fail("bad type");
        return static_cast<Type>(t);
    }

private:
    const char* p_;
    const char* end_;
    bool ok_ = true;
    std::string err_;
};

enum InstBits : uint64_t {
    kHasResult = 1u << 0,
    kHasOperands = 1u << 1,
    kHasImmF64 = 1u << 2,
    kHasImmI32 = 1u << 3,
    kHasCallee = 1u << 4,
    kHasBoxType = 1u << 5,
    kHasKey = 1u << 6,
    kHasIc = 1u << 7,
    kHasDirect = 1u << 8,
    kHasEnvDepth = 1u << 9,
    kHasEnvIndex = 1u << 10,
    kHasEnvHops = 1u << 11,
    kHasSpan = 1u << 12,
    kHasTarget = 1u << 13,
    kHasElse = 1u << 14,
    kEnvImmutable = 1u << 15,
    kHasSlowSite = 1u << 16,
    kHasResumeAfter = 1u << 17,
    kHasInlineScope = 1u << 18,
    kKnownBits = (1u << 19) - 1,
};

bool hasTarget(const BlockTarget& t) { return t.block != kNoBlock || !t.args.empty(); }

void writeTarget(Writer& w, const BlockTarget& t) {
    w.var(static_cast<uint32_t>(t.block + 1u));  // kNoBlock wraps to 0
    w.var(t.args.size());
    for (ValueId a : t.args) w.var(a);
}

void readTarget(Reader& r, BlockTarget& t) {
    t.block = static_cast<BlockId>(r.u32() - 1u);
    t.args.resize(r.count());
    for (auto& a : t.args) a = r.u32();
}

void writeInstruction(Writer& w, const Instruction& inst) {
    uint64_t immBits = 0;
    std::memcpy(&immBits, &inst.immF64, 8);
    uint64_t m = 0;
    if (inst.result != kNoValue) m |= kHasResult;
    if (!inst.operands.empty()) m |= kHasOperands;
    if (immBits != 0) m |= kHasImmF64;
    if (inst.immI32 != 0) m |= kHasImmI32;
    if (inst.calleeIndex != 0) m |= kHasCallee;
    if (inst.boxType != Type::Void) m |= kHasBoxType;
    if (inst.keyIndex != 0) m |= kHasKey;
    if (inst.icIndex != 0) m |= kHasIc;
    if (inst.slowSite != Instruction::kNoSlowSite) m |= kHasSlowSite;
    if (inst.directTarget != Instruction::kNoDirectTarget) m |= kHasDirect;
    if (inst.envDepth != 0) m |= kHasEnvDepth;
    if (inst.envIndex != 0) m |= kHasEnvIndex;
    if (inst.callEnvHops != Instruction::kNoEnvHops) m |= kHasEnvHops;
    if (inst.resumeAfter != Instruction::kNoResumeAfter) m |= kHasResumeAfter;
    if (inst.inlineScope != 0) m |= kHasInlineScope;
    if (inst.span.begin != 0 || inst.span.end != 0 || inst.span.file != 0) m |= kHasSpan;
    if (hasTarget(inst.target)) m |= kHasTarget;
    if (hasTarget(inst.elseTarget)) m |= kHasElse;
    if (inst.envImmutable) m |= kEnvImmutable;

    w.u8(static_cast<uint8_t>(inst.op));
    w.u8(static_cast<uint8_t>(inst.type));
    w.var(m);
    if (m & kHasResult) w.var(inst.result);
    if (m & kHasOperands) {
        w.var(inst.operands.size());
        for (ValueId v : inst.operands) w.var(v);
    }
    if (m & kHasImmF64) w.f64(inst.immF64);
    if (m & kHasImmI32) w.svar(inst.immI32);
    if (m & kHasCallee) w.var(inst.calleeIndex);
    if (m & kHasBoxType) w.u8(static_cast<uint8_t>(inst.boxType));
    if (m & kHasKey) w.var(inst.keyIndex);
    if (m & kHasIc) w.var(inst.icIndex);
    if (m & kHasSlowSite) w.var(inst.slowSite);
    if (m & kHasDirect) w.var(inst.directTarget);
    if (m & kHasEnvDepth) w.var(inst.envDepth);
    if (m & kHasEnvIndex) w.var(inst.envIndex);
    if (m & kHasEnvHops) w.var(inst.callEnvHops);
    if (m & kHasResumeAfter) w.var(inst.resumeAfter);
    if (m & kHasInlineScope) w.var(inst.inlineScope);
    if (m & kHasSpan) {
        w.var(inst.span.begin);
        w.svar(static_cast<int64_t>(inst.span.end) - static_cast<int64_t>(inst.span.begin));
        w.var(inst.span.file);
    }
    if (m & kHasTarget) writeTarget(w, inst.target);
    if (m & kHasElse) writeTarget(w, inst.elseTarget);
}

void readInstruction(Reader& r, Instruction& inst) {
    const uint8_t op = r.u8();
    if (op > static_cast<uint8_t>(kLastOp)) r.fail("bad op");
    inst.op = static_cast<Op>(op);
    inst.type = r.type();
    const uint64_t m = r.var();
    if (m & ~static_cast<uint64_t>(kKnownBits)) r.fail("unknown instruction field");
    if (m & kHasResult) inst.result = r.u32();
    if (m & kHasOperands) {
        inst.operands.resize(r.count());
        for (auto& v : inst.operands) v = r.u32();
    }
    if (m & kHasImmF64) inst.immF64 = r.f64();
    if (m & kHasImmI32) inst.immI32 = r.i32();
    if (m & kHasCallee) inst.calleeIndex = r.u32();
    if (m & kHasBoxType) inst.boxType = r.type();
    if (m & kHasKey) inst.keyIndex = r.u32();
    if (m & kHasIc) inst.icIndex = r.u32();
    if (m & kHasSlowSite) inst.slowSite = r.u32();
    if (m & kHasDirect) inst.directTarget = r.u32();
    if (m & kHasEnvDepth) inst.envDepth = r.u32();
    if (m & kHasEnvIndex) inst.envIndex = r.u32();
    if (m & kHasEnvHops) inst.callEnvHops = r.u32();
    if (m & kHasResumeAfter) inst.resumeAfter = r.u32();
    if (m & kHasInlineScope) inst.inlineScope = r.u32();
    if (m & kHasSpan) {
        inst.span.begin = r.u32();
        const int64_t len = static_cast<int64_t>(inst.span.begin) + static_cast<int64_t>(r.i32());
        if (len < 0 || len > UINT32_MAX) r.fail("bad span");
        inst.span.end = static_cast<uint32_t>(len);
        const uint32_t file = r.u32();
        if (file > UINT16_MAX) r.fail("bad span file");
        inst.span.file = static_cast<uint16_t>(file);
    }
    if (m & kHasTarget) readTarget(r, inst.target);
    if (m & kHasElse) readTarget(r, inst.elseTarget);
    inst.envImmutable = (m & kEnvImmutable) != 0;
}

void writeBlock(Writer& w, const Block& b) {
    w.var(b.id);
    w.var(b.params.size());
    for (const auto& p : b.params) {
        w.var(p.id);
        w.u8(static_cast<uint8_t>(p.type));
    }
    w.u8(b.cold ? 1 : 0);
    w.var(static_cast<uint32_t>(b.handler + 1u));
    w.var(b.instructions.size());
    for (const auto& inst : b.instructions) writeInstruction(w, inst);
}

void readBlock(Reader& r, Block& b) {
    b.id = r.u32();
    b.params.resize(r.count());
    for (auto& p : b.params) {
        p.id = r.u32();
        p.type = r.type();
    }
    b.cold = r.u8() != 0;
    b.handler = static_cast<BlockId>(r.u32() - 1u);
    b.instructions.resize(r.count());
    for (auto& inst : b.instructions) {
        if (!r.ok()) return;
        readInstruction(r, inst);
    }
}

enum FnBits : uint32_t {
    kReturnPinned = 1u << 0,
    kIsExported = 1u << 1,
    kIsExternal = 1u << 2,
    kIsEntryPoint = 1u << 3,
    kNeedsEnv = 1u << 4,
    kNeedsThis = 1u << 5,
    kNeedsArguments = 1u << 6,
    kIsStrict = 1u << 7,
    kIsGenerator = 1u << 8,
    kHasRestParam = 1u << 9,
    kArgumentsIsList = 1u << 10,
    kArgumentsIsCount = 1u << 11,
};

void writeFunction(Writer& w, const Function& fn) {
    uint32_t bits = 0;
    if (fn.returnPinned) bits |= kReturnPinned;
    if (fn.isExported) bits |= kIsExported;
    if (fn.isExternal) bits |= kIsExternal;
    if (fn.isEntryPoint) bits |= kIsEntryPoint;
    if (fn.needsEnv) bits |= kNeedsEnv;
    if (fn.needsThis) bits |= kNeedsThis;
    if (fn.needsArguments) bits |= kNeedsArguments;
    if (fn.isStrict) bits |= kIsStrict;
    if (fn.isGenerator) bits |= kIsGenerator;
    if (fn.hasRestParam) bits |= kHasRestParam;
    if (fn.argumentsIsList) bits |= kArgumentsIsList;
    if (fn.argumentsIsCount) bits |= kArgumentsIsCount;
    w.str(fn.name);
    w.var(bits);
    w.var(fn.params.size());
    for (const auto& p : fn.params) {
        w.str(p.name);
        w.u8(static_cast<uint8_t>(p.type));
        w.u8(p.pinned ? 1u : 0u);
        w.var(p.pinKeyIndex);
    }
    w.u8(static_cast<uint8_t>(fn.returnType));
    w.var(fn.returnPinKeyIndex);
    w.svar(fn.coroKind);
    w.var(fn.fnFlags);
    w.var(fn.requiredArgs);
    w.var(fn.nameKeyIndex);
    w.var(fn.sourceFile);
    w.var(fn.sourceBegin);
    w.var(fn.sourceEnd);
    w.str(fn.displayName);
    w.var(fn.descFlags);
    w.var(static_cast<uint32_t>(fn.speculatedFrom + 1u));  // kNotSpeculated wraps to 0
    w.var(static_cast<uint32_t>(fn.fallback + 1u));        // kNoFallback wraps to 0
    w.var(static_cast<uint32_t>(fn.entrySlowSite + 1u));   // none wraps to 0
    w.var(fn.valueCount);
    w.var(fn.inlineScopes.size());
    for (const auto& s : fn.inlineScopes) {
        w.var(s.callee);
        w.var(s.call.begin);
        w.var(s.call.end);
        w.var(s.call.file);
        w.var(s.parent);
    }
    w.var(fn.blocks.size());
    for (const auto& b : fn.blocks) writeBlock(w, b);
}

void readFunction(Reader& r, Function& fn) {
    fn.name = r.str();
    const uint64_t bits = r.var();
    fn.returnPinned = (bits & kReturnPinned) != 0;
    fn.isExported = (bits & kIsExported) != 0;
    fn.isExternal = (bits & kIsExternal) != 0;
    fn.isEntryPoint = (bits & kIsEntryPoint) != 0;
    fn.needsEnv = (bits & kNeedsEnv) != 0;
    fn.needsThis = (bits & kNeedsThis) != 0;
    fn.needsArguments = (bits & kNeedsArguments) != 0;
    fn.isStrict = (bits & kIsStrict) != 0;
    fn.isGenerator = (bits & kIsGenerator) != 0;
    fn.hasRestParam = (bits & kHasRestParam) != 0;
    fn.argumentsIsList = (bits & kArgumentsIsList) != 0;
    fn.argumentsIsCount = (bits & kArgumentsIsCount) != 0;
    fn.params.resize(r.count());
    for (auto& p : fn.params) {
        p.name = r.str();
        p.type = r.type();
        const uint8_t pflags = r.u8();
        p.pinned = (pflags & 1u) != 0;
        p.pinKeyIndex = r.u32();
    }
    fn.returnType = r.type();
    fn.returnPinKeyIndex = r.u32();
    const int32_t coro = r.i32();
    if (coro < INT8_MIN || coro > INT8_MAX) r.fail("bad coroutine kind");
    fn.coroKind = static_cast<int8_t>(coro);
    fn.fnFlags = r.u32();
    fn.requiredArgs = r.u32();
    fn.nameKeyIndex = r.u32();
    const uint32_t file = r.u32();
    if (file > UINT16_MAX) r.fail("bad source file");
    fn.sourceFile = static_cast<uint16_t>(file);
    fn.sourceBegin = r.u32();
    fn.sourceEnd = r.u32();
    fn.displayName = r.str();
    fn.descFlags = r.u32();
    fn.speculatedFrom = r.u32() - 1u;
    fn.fallback = r.u32() - 1u;
    fn.entrySlowSite = r.u32() - 1u;
    fn.valueCount = r.u32();
    fn.inlineScopes.resize(r.count());
    for (auto& s : fn.inlineScopes) {
        s.callee = r.u32();
        s.call.begin = r.u32();
        s.call.end = r.u32();
        s.call.file = static_cast<uint16_t>(r.u32());
        s.parent = r.u32();
    }
    fn.blocks.resize(r.count());
    for (auto& b : fn.blocks) {
        if (!r.ok()) return;
        readBlock(r, b);
    }
}

constexpr uint32_t kModuleMagic = 0x4C49524Bu;  // "KRIL"

}  // namespace

std::string serializeModule(const Module& module) {
    Writer w;
    w.var(kModuleMagic);
    w.var(kFormatVersion);
    w.str(module.name);
    w.var(module.keyConstants.size());
    for (const auto& k : module.keyConstants) w.str(k);
    w.var(module.icSiteCount);
    w.var(module.templateSiteCount);
    w.var(module.slowSiteCount);
    w.var(module.pretenureSites.size());
    for (uint32_t s : module.pretenureSites) w.var(s);
    w.var(module.mathPinned ? 1u : 0u);
    w.var(module.numberPinned ? 1u : 0u);
    w.var(module.censusSites.size());
    for (const auto& c : module.censusSites) {
        w.var(c.keyIndex);
        w.var(c.info);
    }
    w.str(module.censusOutPath);
    w.var(module.icLayoutDigest);
    w.str(module.harvestOutPath);
    w.var(module.nativeImports.size());
    for (const auto& n : module.nativeImports) {
        w.str(n.name);
        w.str(n.signature);
        w.var(n.functionIndex);
        w.var(n.bufferReturnKind);
    }
    w.var(module.shapeCells.size());
    for (const auto& c : module.shapeCells) w.str(c.key);
    w.var(module.numberSlotKeys.size());
    for (uint32_t k : module.numberSlotKeys) w.var(k);
    w.var(module.objectLiterals.size());
    for (const auto& l : module.objectLiterals) {
        w.var(l.keys.size());
        for (uint32_t k : l.keys) w.var(k);
        w.var(l.inlineSlots);
    }
    w.var(module.protoCells.size());
    for (const auto& c : module.protoCells) {
        w.var(c.fromCell);
        w.var(c.keyIndex);
        w.var(c.toCell);
        w.var(static_cast<uint32_t>(c.slot));
    }
    w.var(module.memberCells.size());
    for (const auto& c : module.memberCells) {
        w.var(static_cast<uint32_t>(c.kind));
        w.var(c.keyIndex);
        w.var(c.callee);
        w.var(c.group);
        w.var(c.groupSize);
    }
    w.var(module.sourceFiles.size());
    for (const auto& f : module.sourceFiles) w.str(f);
    w.var(module.functions.size());
    for (const auto& fn : module.functions) writeFunction(w, fn);
    return w.take();
}

bool deserializeModule(std::string_view bytes, Module& out, std::string& err) {
    Reader r(bytes);
    if (r.var() != kModuleMagic) {
        err = "not an IL module";
        return false;
    }
    if (r.var() != kFormatVersion) {
        err = "IL format version mismatch";
        return false;
    }
    out = Module{};
    out.name = r.str();
    out.keyConstants.resize(r.count());
    for (auto& k : out.keyConstants) k = r.str();
    out.icSiteCount = r.u32();
    out.templateSiteCount = r.u32();
    out.slowSiteCount = r.u32();
    out.pretenureSites.resize(r.count());
    for (auto& s : out.pretenureSites) s = r.u32();
    out.mathPinned = r.u32() != 0;
    out.numberPinned = r.u32() != 0;
    out.censusSites.resize(r.count());
    for (auto& c : out.censusSites) {
        c.keyIndex = r.u32();
        c.info = r.u32();
    }
    out.censusOutPath = r.str();
    out.icLayoutDigest = r.var();
    out.harvestOutPath = r.str();
    out.nativeImports.resize(r.count());
    for (auto& n : out.nativeImports) {
        n.name = r.str();
        n.signature = r.str();
        n.functionIndex = r.u32();
        n.bufferReturnKind = r.u32();
    }
    out.shapeCells.resize(r.count());
    for (auto& c : out.shapeCells) c.key = r.str();
    out.numberSlotKeys.resize(r.count());
    for (auto& k : out.numberSlotKeys) k = r.u32();
    out.objectLiterals.resize(r.count());
    for (auto& l : out.objectLiterals) {
        l.keys.resize(r.count());
        for (uint32_t& k : l.keys) k = r.u32();
        l.inlineSlots = r.u32();
    }
    out.protoCells.resize(r.count());
    for (auto& c : out.protoCells) {
        c.fromCell = r.u32();
        c.keyIndex = r.u32();
        c.toCell = r.u32();
        c.slot = static_cast<int32_t>(r.u32());
    }
    out.memberCells.resize(r.count());
    for (auto& c : out.memberCells) {
        c.kind = static_cast<Module::MemberKind>(r.u32());
        c.keyIndex = r.u32();
        c.callee = r.u32();
        c.group = r.u32();
        c.groupSize = r.u32();
    }
    out.sourceFiles.resize(r.count());
    for (auto& f : out.sourceFiles) f = r.str();
    const size_t fnCount = r.count();
    for (size_t i = 0; i < fnCount && r.ok(); ++i) readFunction(r, out.functions.emplace_back());
    if (r.ok() && !r.atEnd()) r.fail("trailing bytes");
    if (!r.ok()) {
        err = "malformed IL: " + r.error();
        return false;
    }
    return true;
}

void attachSources(Module& module, const std::vector<std::string_view>& texts, bool retainTexts) {
    module.sourceTexts.clear();
    module.lineTables.clear();
    module.lineTables.reserve(texts.size());
    if (retainTexts) module.sourceTexts.reserve(texts.size());
    for (std::string_view text : texts) {
        module.lineTables.emplace_back(text);
        if (retainTexts) module.sourceTexts.emplace_back(text);
    }
}

}  // namespace bronze::il
