#include "runtime/stack_trace.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <unwind.h>
#if defined(__APPLE__)
#include <sys/resource.h>
#endif
#endif

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/heap.h"
#include "runtime/interpreted_frames.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/tls_block.h"

namespace bronze::runtime {

namespace {

struct CodeRangeRegistry {
    std::mutex mu;
    std::atomic<size_t> entry_count{0};
    std::atomic<uintptr_t> min_addr{UINTPTR_MAX};
    std::atomic<uintptr_t> max_addr{0};
    struct RangeEntry {
        uintptr_t start;
        uintptr_t end;
        const bronze_code_range* range;
    };
    std::vector<RangeEntry> entries;
    std::unordered_set<const void*> registered_ptrs;

    void registerRanges(const bronze_code_range* ranges, uint32_t count) {
        if (!ranges || count == 0) return;
        std::lock_guard<std::mutex> lock(mu);
        if (registered_ptrs.count(ranges)) return;
        registered_ptrs.insert(ranges);
        uintptr_t mn = min_addr.load(std::memory_order_relaxed);
        uintptr_t mx = max_addr.load(std::memory_order_relaxed);
        // Sorted insertion: the tiered engine registers its code a function
        // at a time, as it installs it, so a whole-table sort per call would
        // make n installs cost n^2 log n.
        const auto byStart = [](const RangeEntry& a, const RangeEntry& b) { return a.start < b.start; };
        for (uint32_t i = 0; i < count; ++i) {
            if (!ranges[i].code_start || ranges[i].code_size == 0) continue;
            uintptr_t s = reinterpret_cast<uintptr_t>(ranges[i].code_start);
            uintptr_t e = s + ranges[i].code_size;
            if (s < mn) mn = s;
            if (e > mx) mx = e;
            const RangeEntry entry{s, e, &ranges[i]};
            entries.insert(std::upper_bound(entries.begin(), entries.end(), entry, byStart), entry);
        }
        min_addr.store(mn, std::memory_order_release);
        max_addr.store(mx, std::memory_order_release);
        entry_count.store(entries.size(), std::memory_order_release);
    }

    void unregisterRanges(const bronze_code_range* ranges, uint32_t count) {
        if (!ranges) return;
        std::lock_guard<std::mutex> lock(mu);
        registered_ptrs.erase(ranges);
        entries.erase(
            std::remove_if(entries.begin(), entries.end(),
                           [ranges, count](const RangeEntry& re) {
                                return re.range >= ranges && re.range < ranges + count;
                           }),
            entries.end());
        if (entries.empty()) {
            min_addr.store(UINTPTR_MAX, std::memory_order_release);
            max_addr.store(0, std::memory_order_release);
        } else {
            min_addr.store(entries.front().start, std::memory_order_release);
            uintptr_t mx = 0;
            for (const auto& e : entries) if (e.end > mx) mx = e.end;
            max_addr.store(mx, std::memory_order_release);
        }
        entry_count.store(entries.size(), std::memory_order_release);
    }

    const bronze_code_range* find(const void* pc) {
        if (!pc || entry_count.load(std::memory_order_acquire) == 0) return nullptr;
        uintptr_t addr = reinterpret_cast<uintptr_t>(pc);
        if (addr < min_addr.load(std::memory_order_relaxed) ||
            addr >= max_addr.load(std::memory_order_relaxed)) {
            return nullptr;
        }
        std::lock_guard<std::mutex> lock(mu);
        if (entries.empty()) return nullptr;
        auto it = std::upper_bound(
            entries.begin(), entries.end(), addr,
            [](uintptr_t val, const RangeEntry& e) { return val < e.start; });
        if (it == entries.begin()) return nullptr;
        --it;
        if (addr >= it->start && addr < it->end) {
            return it->range;
        }
        return nullptr;
    }
};

static CodeRangeRegistry g_code_ranges;

std::atomic<InterpretedFrameWalker> g_interpreted_walker{nullptr};

// The calling thread's interpreted frames, innermost first (ascending stack
// address), or none without a walker.
std::vector<InterpretedFrame> interpretedFrames() {
    std::vector<InterpretedFrame> out;
    InterpretedFrameWalker walker = g_interpreted_walker.load(std::memory_order_acquire);
    if (!walker) return out;
    out.resize(64);
    size_t n = walker(out.data(), out.size());
    if (n > out.size()) {
        out.resize(n);
        n = walker(out.data(), out.size());
    }
    out.resize(std::min(n, out.size()));
    return out;
}

static inline void get_stack_bounds(uintptr_t& low, uintptr_t& high) {
#if defined(_WIN32)
    NT_TIB* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    if (tib) {
        low = reinterpret_cast<uintptr_t>(tib->StackLimit);
        high = reinterpret_cast<uintptr_t>(tib->StackBase);
        return;
    }
#elif defined(__APPLE__)
    char marker = 0;
    const uintptr_t cur_sp = reinterpret_cast<uintptr_t>(&marker);
    uintptr_t addr = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
    size_t stack_size = pthread_get_stacksize_np(pthread_self());
    if (pthread_main_np()) {
        struct rlimit rl;
        if (getrlimit(RLIMIT_STACK, &rl) == 0) {
            if (rl.rlim_cur >= RLIM_INFINITY || rl.rlim_cur == 0) {
                stack_size = 8 * 1024 * 1024;
            } else {
                stack_size = static_cast<size_t>(rl.rlim_cur);
            }
        } else {
            stack_size = 8 * 1024 * 1024;
        }
    }
    if (addr > cur_sp) {
        high = addr;
        low = (high > stack_size) ? (high - stack_size) : 0;
    } else {
        low = addr;
        high = low + stack_size;
    }
    return;
#elif defined(__linux__)
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        void* stack_addr = nullptr;
        size_t stack_size = 0;
        pthread_attr_getstack(&attr, &stack_addr, &stack_size);
        pthread_attr_destroy(&attr);
        low = reinterpret_cast<uintptr_t>(stack_addr);
        high = low + stack_size;
        return;
    }
#endif
    low = 0;
    high = UINTPTR_MAX;
}

bool lookupProp(ObjectHeader* obj, const std::string& keyStr, Value& out) {
    for (uint32_t depth = 0; depth <= ObjectHeader::kMaxPrototypeDepth; ++depth) {
        ObjectHeader* cur = depth == 0 ? obj : obj->protoAncestor(depth);
        if (!cur) return false;
        if (!cur->shape) continue;
        for (PropertyKey k : cur->shape->ownKeysInInsertionOrder()) {
            if (k.isString() && rtUtf8Chars(k.string()) == keyStr) {
                PropertyInfo info;
                if (cur->shape->lookupProperty(k, info) && !info.accessor) {
                    out = cur->getSlot(info.slot);
                    return true;
                }
                break;
            }
        }
    }
    return false;
}

uint32_t getStackTraceLimit() {
    Value ctor = rtErrorConstructor("Error");
    if (!ctor.isObject() || ctor.asObject<HeapObjectHeader>()->flags != HeapKind::Function) return 10;
    FunctionHeader* fn = ctor.asObject<FunctionHeader>();
    if (!fn->properties.isObject()) return 10;
    ObjectHeader* props = fn->properties.asObject<ObjectHeader>();
    if (!props->shape) return 10;
    for (PropertyKey k : props->shape->ownKeysInInsertionOrder()) {
        if (k.isString() && rtUtf8Chars(k.string()) == "stackTraceLimit") {
            PropertyInfo info;
            if (props->shape->lookupProperty(k, info) && !info.accessor) {
                Value limitVal = props->getSlot(info.slot);
                if (limitVal.isNumber()) {
                    double d = limitVal.asNumber();
                    if (d <= 0.0) return 0;
                    if (d > 10000.0) return 10000;
                    return static_cast<uint32_t>(d);
                }
            }
            break;
        }
    }
    return 10;
}

}  // namespace

void rtSetInterpretedFrameWalker(InterpretedFrameWalker walker) noexcept {
    g_interpreted_walker.store(walker, std::memory_order_release);
}

InterpretedFrameWalker rtGetInterpretedFrameWalker() noexcept {
    return g_interpreted_walker.load(std::memory_order_acquire);
}

void bronze_register_code_ranges_internal(const void* ranges, uint32_t count) {
    g_code_ranges.registerRanges(static_cast<const bronze_code_range*>(ranges), count);
}

void bronze_unregister_code_ranges_internal(const void* ranges, uint32_t count) {
    g_code_ranges.unregisterRanges(static_cast<const bronze_code_range*>(ranges), count);
}

const bronze_code_range* find_code_range(const void* pc) {
    return g_code_ranges.find(pc);
}

bool find_code_site(const void* pc, CodeSite& out) {
    const bronze_code_range* cr = find_code_range(pc);
    if (!cr || !cr->desc) return false;
    out.range = cr;
    out.line = cr->desc->def_line;
    out.col = cr->desc->def_col;
    out.file = cr->desc->file;
    if (cr->pc_table && cr->pc_count > 0) {
        const uintptr_t pc_off =
            reinterpret_cast<uintptr_t>(pc) - reinterpret_cast<uintptr_t>(cr->code_start);
        auto it = std::upper_bound(
            cr->pc_table, cr->pc_table + cr->pc_count, pc_off,
            [](uintptr_t val, const bronze_pc_entry& e) { return val < e.pc_offset; });
        if (it != cr->pc_table) {
            --it;
            out.line = it->line;
            out.col = it->col;
            if (cr->files && it->file < cr->file_count && cr->files[it->file]) {
                out.file = cr->files[it->file];
            }
        }
    }
    return true;
}

std::string bronze_format_stack_trace(Value errorObj, Value skipFn) {
    std::string nameStr = "Error";
    std::string msgStr = "";

    if (errorObj.isObject() && errorObj.asObject<HeapObjectHeader>()->flags == HeapKind::Plain) {
        ObjectHeader* obj = errorObj.asObject<ObjectHeader>();
        Value nameVal;
        if (lookupProp(obj, "name", nameVal) && nameVal.isString()) {
            nameStr = rtUtf8Chars(nameVal.asString<StringHeader>());
        }
        Value msgVal;
        if (lookupProp(obj, "message", msgVal) && msgVal.isString()) {
            msgStr = rtUtf8Chars(msgVal.asString<StringHeader>());
        }
    }

    std::string header;
    if (nameStr.empty()) {
        header = msgStr;
    } else if (msgStr.empty()) {
        header = nameStr;
    } else {
        header = nameStr + ": " + msgStr;
    }

    const uint32_t limit = getStackTraceLimit();
    if (limit == 0) return header;

    uintptr_t stack_low = 0, stack_high = 0;
    get_stack_bounds(stack_low, stack_high);

    auto valid_ptr = [&](const void* p, size_t sz = sizeof(void*)) -> bool {
        uintptr_t a = reinterpret_cast<uintptr_t>(p);
        return (a & 7) == 0 && a >= stack_low && a + sz <= stack_high;
    };

    struct StackFrameInfo {
        const bronze_fn_desc* desc = nullptr;
        uint32_t line = 0;
        uint32_t col = 0;
        const void* code = nullptr;
        const char* builtin_name = nullptr;
        const char* file = nullptr;  // the position's file (CodeSite::file)
    };
    std::vector<StackFrameInfo> frames;

    // Compiled frames are found by walking the native stack; interpreted ones
    // are the interpreter's to report (interpreted_frames.h), each with the
    // stack address of its record. The two lists merge by that address: an
    // interpreted frame whose record lies below a native frame's stack
    // pointer is more recent than that native frame. The walk ends at the
    // first top-level frame of either kind, the program's outermost.
    const size_t cap = limit + 10;
    const std::vector<InterpretedFrame> interpreted = interpretedFrames();
    size_t nextInterpreted = 0;
    bool reachedTop = false;
    const auto done = [&] { return reachedTop || frames.size() >= cap; };
    const auto flushInterpreted = [&](uintptr_t bound) {
        while (!done() && nextInterpreted < interpreted.size() &&
               interpreted[nextInterpreted].stackAddress < bound) {
            const InterpretedFrame& f = interpreted[nextInterpreted++];
            if (!f.desc) continue;
            frames.push_back({f.desc, f.line, f.col, f.desc->code, nullptr, f.file});
            reachedTop = (f.desc->flags & BRONZE_FN_DESC_TOPLEVEL) != 0;
        }
    };
    const auto pushCompiled = [&](const CodeSite& site) {
        frames.push_back({site.range->desc, site.line, site.col, site.range->code_start, nullptr, site.file});
        reachedTop = (site.range->desc->flags & BRONZE_FN_DESC_TOPLEVEL) != 0;
    };

#if defined(_WIN32) && defined(_M_X64)
    CONTEXT ctx;
    RtlCaptureContext(&ctx);

    while (ctx.Rip != 0 && !done()) {
        uintptr_t prev_rip = ctx.Rip;
        uintptr_t prev_rsp = ctx.Rsp;

        // This frame's own locals lie at or above its stack pointer.
        flushInterpreted(ctx.Rsp);
        if (done()) break;

        // The first frame's Rip is the instruction after the call into the
        // runtime, and every later one a return address: the byte before it
        // is the call, and the range and pc-table lookups both see that byte.
        if (CodeSite site; find_code_site(reinterpret_cast<const void*>(ctx.Rip - 1), site)) {
            pushCompiled(site);
            if (done()) break;
        }

        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fnEntry = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (fnEntry) {
            void* fnBegin = reinterpret_cast<void*>(imageBase + fnEntry->BeginAddress);
            const char* builtinName = rtGetNativeDisplayName(fnBegin);
            if (builtinName) {
                frames.push_back({nullptr, 0, 0, nullptr, builtinName});
            }

            void* handlerData = nullptr;
            DWORD64 establisherFrame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fnEntry,
                             &ctx, &handlerData, &establisherFrame, nullptr);
        } else {
            // Leaf function or unrecorded frame: rip = [rsp]; rsp += 8
            if (!valid_ptr(reinterpret_cast<const void*>(ctx.Rsp), 8)) {
                break;
            }
            ctx.Rip = *reinterpret_cast<const uintptr_t*>(ctx.Rsp);
            ctx.Rsp += 8;
        }

        if (ctx.Rip == prev_rip && ctx.Rsp == prev_rsp) {
            break;
        }
    }
#elif !defined(_WIN32)
    // The frame-pointer chain. Compiled code keeps rbp/x29 in every non-leaf
    // prologue, the enter_js trampoline keeps it, and the runtime — the only
    // C++ that sits between two compiled frames — is built with
    // -fno-omit-frame-pointer (src/runtime/CMakeLists.txt), so each link's
    // saved return address is the pc to attribute. The pc is the address
    // after the call, so the byte before it is what the lookups see: a call
    // that ends a function (a throw, say) returns to the next function's
    // first byte.
    //
    // A builtin is named by the start of the function holding the pc, which
    // the unwinder's FDE lookup answers the way RtlLookupFunctionEntry does
    // above. dladdr cannot: it knows dynamic symbols only, and the shared
    // runtime exports the ABI and nothing else, so it would name the nearest
    // export before the pc rather than the builtin itself.
    void* cur_rbp = __builtin_frame_address(0);
    while (valid_ptr(cur_rbp, 16) && !done()) {
        uintptr_t* fp = static_cast<uintptr_t*>(cur_rbp);
        uintptr_t caller_rbp = fp[0];
        uintptr_t caller_rip = fp[1];
        if (caller_rip == 0) break;
        void* call_pc = reinterpret_cast<void*>(caller_rip - 1);

        // The caller's locals lie above the saved frame pointer and return
        // address; everything below them is more recent than the caller.
        flushInterpreted(reinterpret_cast<uintptr_t>(cur_rbp) + 16);
        if (done()) break;

        if (CodeSite site; find_code_site(call_pc, site)) {
            pushCompiled(site);
            if (done()) break;
        } else if (void* fnBegin = _Unwind_FindEnclosingFunction(call_pc)) {
            const char* builtinName = rtGetNativeDisplayName(fnBegin);
            if (builtinName) {
                frames.push_back({nullptr, 0, 0, nullptr, builtinName});
            }
        }

        if (caller_rbp <= reinterpret_cast<uintptr_t>(cur_rbp) || caller_rbp >= stack_high) {
            break;
        }
        cur_rbp = reinterpret_cast<void*>(caller_rbp);
    }
#endif
    // Interpreted frames older than every native frame the walk reached.
    flushInterpreted(UINTPTR_MAX);

    size_t start_idx = 0;
    if (skipFn.isObject() && skipFn.asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
        FunctionHeader* fn = skipFn.asObject<FunctionHeader>();
        const void* targetCode = reinterpret_cast<const void*>(fn->code);
        bool found = false;
        for (size_t i = 0; i < frames.size(); ++i) {
            if (frames[i].desc && targetCode != nullptr) {
                if (frames[i].desc->code == targetCode || frames[i].code == targetCode) {
                    start_idx = i + 1;
                    found = true;
                    break;
                }
                const bronze_code_range* cr = find_code_range(targetCode);
                if (cr && cr->desc == frames[i].desc) {
                    start_idx = i + 1;
                    found = true;
                    break;
                }
            }
        }
        if (!found) return header;
    }

    std::string out = header;
    uint32_t count = 0;
    for (size_t i = start_idx; i < frames.size() && count < limit; ++i) {
        const auto& fi = frames[i];
        if (fi.builtin_name) {
            out += "\n    at " + std::string(fi.builtin_name) + " (<anonymous>)";
            ++count;
            continue;
        }
        const bronze_fn_desc* desc = fi.desc;
        if (!desc) continue;

        std::string lineStr;
        bool isAnon = ((desc->flags & BRONZE_FN_DESC_TOPLEVEL) || !desc->name ||
                       desc->name[0] == '\0' || strcmp(desc->name, "<anonymous>") == 0);
        const char* file = fi.file ? fi.file : desc->file;
        std::string fileStr = (file && file[0] != '\0') ? file : "<anonymous>";
        uint32_t line = fi.line ? fi.line : desc->def_line;
        uint32_t col = fi.col ? fi.col : desc->def_col;
        if (line == 0) line = 1;
        if (col == 0) col = 1;

        if (desc->flags & BRONZE_FN_DESC_BUILTIN) {
            lineStr = "    at " + std::string(desc->name ? desc->name : "<anonymous>") +
                      " (<anonymous>)";
        } else if (isAnon) {
            if (fileStr == "<anonymous>") {
                lineStr = "    at <anonymous>";
            } else {
                lineStr = "    at " + fileStr + ":" + std::to_string(line) + ":" +
                          std::to_string(col);
            }
        } else {
            std::string fnName;
            if (desc->flags & BRONZE_FN_DESC_CONSTRUCTOR) {
                fnName = "new " + std::string(desc->name);
            } else {
                fnName = desc->name;
            }
            if (fileStr == "<anonymous>") {
                lineStr = "    at " + fnName + " (<anonymous>)";
            } else {
                lineStr = "    at " + fnName + " (" + fileStr + ":" + std::to_string(line) +
                          ":" + std::to_string(col) + ")";
            }
        }
        out += "\n" + lineStr;
        ++count;
    }
    return out;
}

void bronze_install_stack(Value errorObj, Value skipFn) {
    if (!errorObj.isObject()) return;
    Rooted<Value> self{errorObj};
    std::string stackStr = bronze_format_stack_trace(self.get(), skipFn);
    Rooted<Value> key{rtMakeString("stack")};
    Rooted<Value> val{rtMakeString(stackStr)};
    self.get().asObject<ObjectHeader>()->setProp(
        rtHeap(), rtArena(), key, val,
        /*ic=*/nullptr, /*enumerable=*/false, /*defineOwn=*/true,
        /*receiver=*/nullptr, /*refused=*/nullptr,
        /*writable=*/true, /*configurable=*/true);
}

}  // namespace bronze::runtime

extern "C" void bronze_register_code_ranges(const void* ranges, uint32_t count) {
    bronze::runtime::bronze_register_code_ranges_internal(ranges, count);
}

extern "C" void bronze_unregister_code_ranges(const void* ranges, uint32_t count) {
    bronze::runtime::bronze_unregister_code_ranges_internal(ranges, count);
}
