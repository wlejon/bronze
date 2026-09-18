#include "runtime/stack_trace.h"

#include <algorithm>
#include <cstdint>
#include <string>

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

    bronze_call_frame* cur = rtTls()->call_frame_top;
    if (!cur) return header;

    if (skipFn.isObject() && skipFn.asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
        FunctionHeader* fn = skipFn.asObject<FunctionHeader>();
        const void* targetCode = reinterpret_cast<const void*>(fn->code);
        bronze_call_frame* match = nullptr;
        for (bronze_call_frame* f = cur; f != nullptr; f = f->prev) {
            if (f->desc && targetCode != nullptr && f->desc->code == targetCode) {
                match = f;
                break;
            }
        }
        if (!match) return header;
        cur = match->prev;
    } else if (skipFn.isUndefined() && errorObj.isObject() &&
               errorObj.asObject<HeapObjectHeader>()->flags == HeapKind::Plain) {
        ObjectHeader* obj = errorObj.asObject<ObjectHeader>();
        Value ctorVal;
        if (lookupProp(obj, "constructor", ctorVal) && ctorVal.isObject() &&
            ctorVal.asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
            FunctionHeader* ctorFn = ctorVal.asObject<FunctionHeader>();
            if (ctorFn && ctorFn->name) {
                std::string ctorName = rtUtf8Chars(ctorFn->name);
                if (cur && cur->desc && (cur->desc->flags & BRONZE_FN_DESC_CONSTRUCTOR) &&
                    cur->desc->name && ctorName == cur->desc->name) {
                    cur = cur->prev;
                }
            }
        }
    }

    std::string out = header;
    uint32_t count = 0;
    while (cur && count < limit) {
        const bronze_fn_desc* desc = cur->desc;
        if (desc) {
            std::string lineStr;
            bool isAnon = ((desc->flags & BRONZE_FN_DESC_TOPLEVEL) || !desc->name ||
                           desc->name[0] == '\0' || strcmp(desc->name, "<anonymous>") == 0);
            std::string fileStr =
                (desc->file && desc->file[0] != '\0') ? desc->file : "<anonymous>";
            uint32_t line = cur->call_line ? cur->call_line : desc->def_line;
            uint32_t col = cur->call_col ? cur->call_col : desc->def_col;
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
        cur = cur->prev;
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
