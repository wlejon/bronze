#include "runtime/stack_trace.h"

#include <algorithm>
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
#endif

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/heap.h"
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
        for (uint32_t i = 0; i < count; ++i) {
            if (!ranges[i].code_start || ranges[i].code_size == 0) continue;
            uintptr_t s = reinterpret_cast<uintptr_t>(ranges[i].code_start);
            uintptr_t e = s + ranges[i].code_size;
            if (s < mn) mn = s;
            if (e > mx) mx = e;
            entries.push_back({s, e, &ranges[i]});
        }
        std::sort(entries.begin(), entries.end(), [](const RangeEntry& a, const RangeEntry& b) {
            return a.start < b.start;
        });
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

static inline void get_stack_bounds(uintptr_t& low, uintptr_t& high) {
#if defined(_WIN32)
    NT_TIB* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    if (tib) {
        low = reinterpret_cast<uintptr_t>(tib->StackLimit);
        high = reinterpret_cast<uintptr_t>(tib->StackBase);
        return;
    }
#endif
    low = 0;
    high = UINTPTR_MAX;
}

inline void bronze_get_context(void** out_rsp, void** out_rbp) {
#if defined(_WIN32)
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    if (out_rsp) *out_rsp = reinterpret_cast<void*>(ctx.Rsp);
    if (out_rbp) *out_rbp = reinterpret_cast<void*>(ctx.Rbp);
#else
    if (out_rsp) {
        void* sp = nullptr;
        __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
        *out_rsp = sp;
    }
    if (out_rbp) {
        *out_rbp = __builtin_frame_address(0);
    }
#endif
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

void bronze_register_code_ranges_internal(const void* ranges, uint32_t count) {
    g_code_ranges.registerRanges(static_cast<const bronze_code_range*>(ranges), count);
}

void bronze_unregister_code_ranges_internal(const void* ranges, uint32_t count) {
    g_code_ranges.unregisterRanges(static_cast<const bronze_code_range*>(ranges), count);
}

const bronze_code_range* find_code_range(const void* pc) {
    return g_code_ranges.find(pc);
}

void* find_current_js_rbp() {
    void* cur_rsp = nullptr;
    bronze_get_context(&cur_rsp, nullptr);
    if (!cur_rsp) return nullptr;

    uintptr_t stack_low = 0, stack_high = 0;
    get_stack_bounds(stack_low, stack_high);

    auto valid_ptr = [&](const void* p, size_t sz = sizeof(void*)) -> bool {
        uintptr_t a = reinterpret_cast<uintptr_t>(p);
        return (a & 7) == 0 && a >= stack_low && a + sz <= stack_high;
    };

    if (!valid_ptr(cur_rsp)) return nullptr;

    void** sp = static_cast<void**>(cur_rsp);
    while (valid_ptr(sp)) {
        void* ret = *sp;
        uintptr_t a = reinterpret_cast<uintptr_t>(ret);
        const bronze_code_range* r = find_code_range(ret);
        if (r && r->desc && a > reinterpret_cast<uintptr_t>(r->code_start)) {
            return static_cast<void*>(sp);
        }
        ++sp;
    }
    return nullptr;
}

EntryLinkGuard::EntryLinkGuard(const bronze_fn_desc* builtin_desc) {
    bronze_tls_block* tls = rtTls();
    old_top = tls->entry_link_top;
    link.prev = old_top;
    link.builtin_desc = builtin_desc;
    link.js_rbp = find_current_js_rbp();
    tls->entry_link_top = &link;
}

EntryLinkGuard::~EntryLinkGuard() {
    rtTls()->entry_link_top = old_top;
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

    void* cur_rsp = nullptr;
    bronze_get_context(&cur_rsp, nullptr);

    uintptr_t stack_low = 0, stack_high = 0;
    get_stack_bounds(stack_low, stack_high);

    auto valid_ptr = [&](const void* p, size_t sz = sizeof(void*)) -> bool {
        uintptr_t a = reinterpret_cast<uintptr_t>(p);
        return (a & 7) == 0 && a >= stack_low && a + sz <= stack_high;
    };

    struct StackFrameInfo {
        const bronze_fn_desc* desc;
        uint32_t line;
        uint32_t col;
        const void* code;
    };
    std::vector<StackFrameInfo> frames;
    const bronze_entry_link* entry_link = rtTls()->entry_link_top;

    if (cur_rsp && valid_ptr(cur_rsp)) {
        void** sp = static_cast<void**>(cur_rsp);
        void** last_frame_sp = nullptr;
        while (valid_ptr(sp) && frames.size() < limit + 10) {
            while (entry_link && entry_link->js_rbp && sp >= static_cast<void**>(entry_link->js_rbp)) {
                if (entry_link->builtin_desc) {
                    frames.push_back({entry_link->builtin_desc, 0, 0, nullptr});
                }
                entry_link = entry_link->prev;
            }

            void* ret = *sp;
            uintptr_t a = reinterpret_cast<uintptr_t>(ret);
            const bronze_code_range* r = find_code_range(ret);
            if (r && r->desc && a > reinterpret_cast<uintptr_t>(r->code_start)) {
                // Deduplicate if the same function was recorded within 4 stack words
                if (frames.empty() || frames.back().desc != r->desc || (last_frame_sp && (sp - last_frame_sp) >= 4)) {
                    uint32_t f_line = r->desc->def_line;
                    uint32_t f_col = r->desc->def_col;
                    if (r->pc_table && r->pc_count > 0) {
                        uintptr_t pc_off = a - 1 - reinterpret_cast<uintptr_t>(r->code_start);
                        auto it = std::upper_bound(
                            r->pc_table, r->pc_table + r->pc_count, pc_off,
                            [](uintptr_t val, const bronze_pc_entry& e) { return val < e.pc_offset; });
                        if (it != r->pc_table) {
                            --it;
                            f_line = it->line;
                            f_col = it->col;
                        }
                    }
                    frames.push_back({r->desc, f_line, f_col, r->code_start});
                    last_frame_sp = sp;

                    if (r->desc->flags & BRONZE_FN_DESC_TOPLEVEL) {
                        break;
                    }
                }
            }
            ++sp;
        }
    }

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
    } else if (skipFn.isUndefined() && errorObj.isObject() &&
               errorObj.asObject<HeapObjectHeader>()->flags == HeapKind::Plain) {
        ObjectHeader* obj = errorObj.asObject<ObjectHeader>();
        Value ctorVal;
        if (lookupProp(obj, "constructor", ctorVal) && ctorVal.isObject() &&
            ctorVal.asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
            FunctionHeader* ctorFn = ctorVal.asObject<FunctionHeader>();
            if (ctorFn && ctorFn->name && !frames.empty()) {
                std::string ctorName = rtUtf8Chars(ctorFn->name);
                const bronze_fn_desc* d = frames[0].desc;
                if (d && (d->flags & BRONZE_FN_DESC_CONSTRUCTOR) && d->name &&
                    ctorName == d->name) {
                    start_idx = 1;
                }
            }
        }
    }

    std::string out = header;
    uint32_t count = 0;
    for (size_t i = start_idx; i < frames.size() && count < limit; ++i) {
        const auto& fi = frames[i];
        const bronze_fn_desc* desc = fi.desc;
        if (!desc) continue;

        std::string lineStr;
        bool isAnon = ((desc->flags & BRONZE_FN_DESC_TOPLEVEL) || !desc->name ||
                       desc->name[0] == '\0' || strcmp(desc->name, "<anonymous>") == 0);
        std::string fileStr = (desc->file && desc->file[0] != '\0') ? desc->file : "<anonymous>";
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
