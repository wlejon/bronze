// The build half of interpreted cold functions (cold_companion.h): which
// functions, the companion's blob, and what the image adds for it.

#include "codegen-brass/cold_companion.h"

#include "abi/bronze_abi.h"
#include "codegen-brass/brass_backend.h"
#include "codegen-brass/brass_backend_sections.h"
#include "codegen-brass/il2mir/il_abi.h"
#include "codegen-brass/size_report.h"
#include "il/serialize.h"
#include "support/diagnostics.h"
#include "support/source.h"

#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>
#include <brass/object/object_writer.hpp>
#include <brass/runtime/multi_tier_pipeline.hpp>

#include <cstdio>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace bronze::codegen {

namespace {

using brass::object::ObjectFile;
using brass::object::RelocKind;
using brass::object::Section;
using brass::object::SymbolBinding;
using brass::object::SymbolType;

constexpr uint64_t kBlobMagic = 0x504D4F43444C4F43ull;  // "COLDCOMP"

bool hasOp(const il::Function& fn, std::initializer_list<il::Op> ops) {
    for (const il::Block& b : fn.blocks) {
        for (const il::Instruction& inst : b.instructions) {
            for (il::Op op : ops) {
                if (inst.op == op) return true;
            }
        }
    }
    return false;
}

uint64_t instructionCount(const il::Function& fn) {
    uint64_t n = 0;
    for (const il::Block& b : fn.blocks) n += b.instructions.size();
    return n;
}

class Writer {
public:
    void var(uint64_t v) {
        while (v >= 0x80) {
            out_.push_back(static_cast<char>((v & 0x7F) | 0x80));
            v >>= 7;
        }
        out_.push_back(static_cast<char>(v));
    }
    void bytes(std::string_view s) {
        var(s.size());
        out_.append(s.data(), s.size());
    }
    void list(const std::vector<uint32_t>& v) {
        var(v.size());
        for (uint32_t x : v) var(x);
    }
    void pairs(const std::vector<std::pair<uint32_t, uint32_t>>& v) {
        var(v.size());
        for (const auto& [a, b] : v) {
            var(a);
            var(b);
        }
    }
    std::string take() { return std::move(out_); }

private:
    std::string out_;
};

class Reader {
public:
    explicit Reader(std::string_view b) : p_(b.data()), end_(b.data() + b.size()) {}
    bool ok() const { return ok_; }
    bool atEnd() const { return p_ == end_; }
    uint64_t var() {
        uint64_t v = 0;
        for (int shift = 0; shift < 64 && p_ < end_; shift += 7) {
            const uint8_t b = static_cast<uint8_t>(*p_++);
            v |= static_cast<uint64_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) return v;
        }
        ok_ = false;
        return 0;
    }
    uint32_t u32() {
        const uint64_t v = var();
        if (v > UINT32_MAX) ok_ = false;
        return static_cast<uint32_t>(v);
    }
    size_t count() {
        const uint64_t n = var();
        if (n > static_cast<uint64_t>(end_ - p_)) {
            ok_ = false;
            return 0;
        }
        return static_cast<size_t>(n);
    }
    std::string_view bytes() {
        const size_t n = count();
        std::string_view s(p_, ok_ ? n : 0);
        p_ += s.size();
        return s;
    }
    void list(std::vector<uint32_t>& v) {
        v.resize(count());
        for (uint32_t& x : v) x = u32();
    }
    void pairs(std::vector<std::pair<uint32_t, uint32_t>>& v) {
        v.resize(count());
        for (auto& [a, b] : v) {
            a = u32();
            b = u32();
        }
    }

private:
    const char* p_;
    const char* end_;
    bool ok_ = true;
};

void defineLocal(ObjectFile& obj, std::string name, int32_t section, size_t off, size_t size) {
    obj.add_symbol({std::move(name), section, off, size, SymbolBinding::Local, SymbolType::Object});
}

void emitPointer(Section& sec, const std::string& sym) {
    if (!sym.empty()) sec.relocations.push_back({sec.data.size(), RelocKind::Abs64, sym, 0});
    sec.emit64(0);
}

// The names the companion's code refers to and does not define, which the
// image or the runtime must.
std::set<std::string> referencedNames(const brass::Module& mod) {
    std::unordered_set<std::string_view> defined;
    for (const brass::Function* fn : mod.functions()) {
        if (fn && fn->has_body()) defined.insert(fn->name());
    }
    for (const auto& [sym, text] : mod.string_symbols()) defined.insert(sym);
    std::set<std::string> names;
    for (const brass::Function* fn : mod.functions()) {
        if (!fn || !fn->has_body()) continue;
        for (const brass::BasicBlock* bb : fn->blocks()) {
            for (const brass::Instruction* inst : *bb) {
                for (std::string_view s : {inst->symbol(), inst->extra_symbol()}) {
                    if (!s.empty() && !defined.count(s)) names.emplace(s);
                }
            }
        }
    }
    return names;
}

}  // namespace

ColdSelection selectInterpreted(const il::Module& module, const std::vector<std::string>& uniqueNames) {
    ColdSelection sel;
    sel.interpreted.assign(module.functions.size(), UINT32_MAX);
    for (size_t i = 0; i < module.functions.size(); ++i) {
        const il::Function& fn = module.functions[i];
        // Only what the harvested run never entered: a function it entered a
        // few times is start-up code, which runs again at every launch, and
        // its native cold code starts sooner than the interpreter does.
        if (fn.heat != il::Function::Heat::Never) continue;
        if (fn.blocks.empty() || fn.isExternal || fn.isEntryPoint || uniqueNames[i] == "main") continue;
        // A program's top-level code runs once, at start, where lowering it
        // for the interpreter would cost more than its native code does.
        if ((fn.descFlags & BRONZE_FN_DESC_TOPLEVEL) != 0) {
            ++sel.toplevel.functions;
            sel.toplevel.instructions += instructionCount(fn);
            continue;
        }
        // brass lowers coroutines over a whole module before it runs, and a
        // resume twin is cut from its function's body: the image keeps both.
        if (fn.isCoroutineBody() || hasOp(fn, {il::Op::CoroStart, il::Op::CoroSuspend})) {
            ++sel.coroutine.functions;
            sel.coroutine.instructions += instructionCount(fn);
            continue;
        }
        if (fn.isSpeculated() || fn.isOptimistic() ||
            hasOp(fn, {il::Op::ResumePoint, il::Op::Leave, il::Op::ResumeCall})) {
            ++sel.speculation.functions;
            sel.speculation.instructions += instructionCount(fn);
            continue;
        }
        // Compiled code calls a Tier-0 function through a bridge of so many
        // arguments.
        if (fn.params.size() > brass::runtime::MultiTierPipeline::kTier0BridgeMaxParams) {
            ++sel.params.functions;
            sel.params.instructions += instructionCount(fn);
            continue;
        }
        sel.interpreted[i] = sel.cells++;
    }
    return sel;
}

std::string encodeCompanion(const il::Module& module, const ColdCompanionInputs& in) {
    // Only the interpreted bodies: every other function is a declaration,
    // one with no body in the image an import (raw bits, as there).
    il::Module stripped = module;
    for (size_t i = 0; i < stripped.functions.size(); ++i) {
        if (i < in.interpreted.size() && in.interpreted[i] != UINT32_MAX) continue;
        il::Function& fn = stripped.functions[i];
        fn.isExternal = fn.isExternal || fn.blocks.empty();
        fn.blocks.clear();
        fn.blocks.shrink_to_fit();
        fn.inlineScopes.clear();
        // What only the image's descriptor and wrapper read.
        fn.displayName.clear();
        for (il::Param& p : fn.params) p.name.clear();
    }
    Writer w;
    w.var(kBlobMagic);
    w.bytes(il::serializeModule(stripped));
    w.list(in.interpreted);
    w.list(in.globalReadKeys);
    w.list(in.elemCellSites);
    w.list(in.methodIcSites);
    w.list(in.fnFlags);
    w.pairs(in.speculated);
    w.pairs(in.fallbacks);
    w.var(in.perThreadData ? 1 : 0);
    w.var(module.lineTables.size());
    for (const LineTable& t : module.lineTables) {
        w.list(t.lineStarts());
        w.var(t.size());
    }
    return w.take();
}

bool decodeCompanion(std::string_view bytes, il::Module& module, ColdCompanionInputs& in, std::string& err) {
    Reader r(bytes);
    if (r.var() != kBlobMagic) {
        err = "not a cold companion";
        return false;
    }
    if (!il::deserializeModule(r.bytes(), module, err)) return false;
    r.list(in.interpreted);
    r.list(in.globalReadKeys);
    r.list(in.elemCellSites);
    r.list(in.methodIcSites);
    r.list(in.fnFlags);
    r.pairs(in.speculated);
    r.pairs(in.fallbacks);
    in.perThreadData = r.var() != 0;
    module.lineTables.resize(r.count());
    for (LineTable& t : module.lineTables) {
        std::vector<uint32_t> starts;
        r.list(starts);
        t = LineTable::fromLineStarts(std::move(starts), r.u32());
    }
    if (!r.ok() || !r.atEnd() || in.interpreted.size() != module.functions.size() ||
        in.fnFlags.size() != module.functions.size()) {
        err = "malformed cold companion";
        return false;
    }
    return true;
}

bool emitColdCompanion(ObjectFile& obj, const BrassBackend& backend, const il::Module& module,
                       const std::vector<std::string>& uniqueNames, const ColdCompanionInputs& inputs,
                       std::string& why) {
    const std::string& entry = backend.entrySymbol();
    const std::string blob = encodeCompanion(module, inputs);

    // The companion as the resolver will build it, from the blob, every body
    // at once: what it references, and that it lowers at all.
    il::Module companionIl;
    ColdCompanionInputs decoded;
    if (!decodeCompanion(blob, companionIl, decoded, why)) return false;
    BrassBackend cb = backend;
    cb.setColdCompanion(&decoded);
    cb.setOptimize(false);
    cb.setLazyBodies(false);
    cb.setInProcess(true);
    cb.setHarvestEvidence(false);
    DiagnosticSink sink;
    std::unique_ptr<brass::Module> mir = cb.buildMirModule(companionIl, sink);
    if (!mir) {
        why = sink.all().empty() ? "its MIR did not build" : sink.all().front().message;
        return false;
    }
    std::set<std::string> names = referencedNames(*mir);
    mir.reset();
    // What the resolver itself reads: each interpreted function's descriptor
    // and every inlined callee's (TieredProgramImage::descriptorOf), and the
    // file table.
    for (size_t i = 0; i < module.functions.size(); ++i) {
        if (inputs.interpreted[i] == UINT32_MAX) continue;
        names.insert(moduleSymbolName(entry, "__bronze_fn_desc_" + uniqueNames[i]));
        for (const il::Function::InlineScope& s : module.functions[i].inlineScopes) {
            if (s.callee < module.functions.size() && !module.functions[s.callee].blocks.empty()) {
                names.insert(moduleSymbolName(entry, "__bronze_fn_desc_" + uniqueNames[s.callee]));
            }
        }
    }
    if (!module.sourceFiles.empty()) names.insert(moduleSymbolName(entry, "__bronze_file_table"));

    // Each name the image defines goes in the link table, under the name the
    // companion uses; the rest the runtime must define, as it does for the
    // image's own references.
    const std::string keySymbol = entry == "bronze_main" ? "bronze_main_key_constants" : entry + "_key_constants";
    std::unordered_map<std::string_view, const brass::object::ObjectSymbol*> defined;
    std::unordered_set<std::string_view> imported;
    for (const auto& sym : obj.symbols) {
        if (sym.section_index >= 0) {
            defined.emplace(sym.name, &sym);
        } else {
            imported.insert(sym.name);
        }
    }
    std::vector<std::pair<std::string, std::string>> links;  // companion's name, image's
    for (const std::string& name : names) {
        const std::string target = name == "bronze_main_key_constants" ? keySymbol : name;
        if (defined.count(target)) {
            links.emplace_back(name, target);
        } else if (!imported.count(name) && name.rfind("bronze_", 0) != 0 && name.rfind("brass_", 0) != 0) {
            why = "it refers to '" + name + "', which neither the image nor the runtime defines";
            return false;
        }
    }

    // Both sections exist by now (emitBronzeSections); referenced once both
    // are looked up, as a section added would move the others.
    const std::string roName = obj.target.is_windows() ? ".rdata" : (obj.target.is_macos() ? "__const" : ".rodata");
    obj.get_or_create_section(roName, brass::object::SectionKind::RoData,
                              brass::object::SectionFlags::Read | brass::object::SectionFlags::Alloc, 16);
    obj.get_or_create_section(
        ".data", brass::object::SectionKind::Data,
        brass::object::SectionFlags::Read | brass::object::SectionFlags::Write | brass::object::SectionFlags::Alloc,
        8);
    const int32_t roIdx = obj.get_section_index(roName);
    const int32_t dataIdx = obj.get_section_index(".data");
    Section& ro = obj.sections[static_cast<size_t>(roIdx)];
    Section& data = obj.sections[static_cast<size_t>(dataIdx)];

    // The cells: zero, shared by every thread (after the per-thread run).
    const uint32_t cellCount = [&] {
        uint32_t n = 0;
        for (uint32_t c : inputs.interpreted) n += c != UINT32_MAX;
        return n;
    }();
    const std::string cellsSym = moduleSymbolName(entry, std::string(il2mir::kColdCellsSymbol));
    data.align_to(8);
    const size_t cellsOff = data.data.size();
    data.data.resize(cellsOff + size_t{cellCount} * 8, 0);
    defineLocal(obj, cellsSym, dataIdx, cellsOff, size_t{cellCount} * 8);

    const std::string blobSym = moduleSymbolName(entry, "__bronze_cold_blob");
    ro.align_to(8);
    const size_t blobOff = ro.data.size();
    ro.emit_bytes(reinterpret_cast<const uint8_t*>(blob.data()), blob.size());
    defineLocal(obj, blobSym, roIdx, blobOff, blob.size());

    const std::string namesSym = moduleSymbolName(entry, "__bronze_cold_link_names");
    const size_t namesOff = ro.data.size();
    for (const auto& [name, target] : links) {
        ro.emit_bytes(reinterpret_cast<const uint8_t*>(name.data()), name.size());
        ro.emit8(0);
    }
    const std::string entryNameSym = moduleSymbolName(entry, "__bronze_cold_entry_name");
    const size_t entryNameOff = ro.data.size();
    ro.emit_bytes(reinterpret_cast<const uint8_t*>(entry.data()), entry.size());
    ro.emit8(0);
    defineLocal(obj, namesSym, roIdx, namesOff, entryNameOff - namesOff);
    defineLocal(obj, entryNameSym, roIdx, entryNameOff, entry.size() + 1);

    const std::string addrsSym = moduleSymbolName(entry, "__bronze_cold_link_addrs");
    ro.align_to(8);
    const size_t addrsOff = ro.data.size();
    for (const auto& [name, target] : links) emitPointer(ro, target);
    defineLocal(obj, addrsSym, roIdx, addrsOff, links.size() * 8);

    ro.align_to(8);
    const size_t linkOff = ro.data.size();
    ro.emit64(kColdLinkMagic);
    emitPointer(ro, blobSym);
    ro.emit64(blob.size());
    emitPointer(ro, cellsSym);
    ro.emit64(cellCount);
    emitPointer(ro, namesSym);
    emitPointer(ro, addrsSym);
    ro.emit64(links.size());
    emitPointer(ro, entryNameSym);
    static_assert(sizeof(ColdLink) == 9 * 8, "ColdLink is the record emitted here");
    if (sizeReportEnabled()) {
        std::fprintf(stderr, "cold: companion IL %.2f MB, %zu links (%.2f MB)\n", blob.size() / 1048576.0,
                     links.size(), (ro.data.size() - namesOff) / 1048576.0);
    }
    defineLocal(obj, moduleSymbolName(entry, std::string(il2mir::kColdLinkSymbol)), roIdx, linkOff, sizeof(ColdLink));
    return true;
}

}  // namespace bronze::codegen
