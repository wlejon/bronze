#pragma once

// The module's property stubs (il_prop_stubs.cpp): a named read's or write's
// path once its inline way-0 test has missed, built once per module and
// called with the helper's own operands, which they call in turn for
// anything they do not answer.

#include "il_ast.h"
#include <brass/mir/builder.hpp>
#include <brass/mir/function.hpp>

#include <string>

namespace il2mir {

// (i64 obj, i32 key, ptr site) -> i64, behind bronze_prop_get.
constexpr const char* kPropGetStub = "__bronze_prop_get_stub";
void build_prop_get_stub(Function* fn);

// (i64 obj, i32 key, i64 val, ptr site, i32 mode) -> void, behind
// bronze_prop_set / bronze_prop_define.
// A setter it calls is entered through the stub named `setter_call_sym`.
constexpr const char* kPropSetStub = "__bronze_prop_set_stub";
void build_prop_set_stub(Function* fn, const std::string& setter_call_sym);
// (i64 setter payload, i64 this, i64 val) -> void: the setter entered with
// its one argument rooted.
constexpr const char* kSetterCallStub = "__bronze_setter_call_stub";
void build_setter_call_stub(Function* fn);

// (i64 recv, i32 key, i32 argc, ptr argv, ptr site) -> i64, behind
// bronze_call_method: a method site's way 1 and the megamorphic table.
constexpr const char* kMethodCallStub = "__bronze_method_call_stub";
void build_method_call_stub(Function* fn);

// A typed array's element (il_view_stubs.cpp), behind the helpers:
// (i64 obj, i32 index, i64 key) -> i64 and
// (i64 obj, i32 index, i64 key, i64 val, i32 strict) -> void.
constexpr const char* kViewGetStub = "__bronze_view_get_stub";
void build_view_get_stub(Function* fn);
// The first byte of typed-array view `ptr`'s window (its buffer's store plus
// its byteOffset): what an element access indexes.
Value* view_data(Builder& b, Value* ptr);
// Element `idx` (i64) of a view of element kind `kind` at `data`, as
// `typed.load` gives it (il_op.h): f64 for Float32, Float64 and Uint32, i32
// for the other integer kinds. Not for Float16 or the BigInt kinds.
Value* view_element_load(Builder& b, Value* data, Value* idx, int kind);
// Stores `v` as element `idx`, as `typed.store` takes it: an f64 for the
// float kinds and for Uint8Clamped (rounded half to even into [0, 255], NaN
// to 0), an i32 whose low bits the integer kinds keep.
void view_element_store(Builder& b, Value* data, Value* idx, int kind, Value* v);
constexpr const char* kViewSetStub = "__bronze_view_set_stub";
void build_view_set_stub(Function* fn);

}  // namespace il2mir
