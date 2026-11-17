// An image's interpreted cold functions (TranslatorOptions::interpreted;
// codegen-brass/cold_companion.h): the stub each has in place of its body.

#include "il_abi.h"
#include "il_lowering.h"

namespace il2mir {

// The stub has the function's signature, so every caller (a direct call, the
// wrapper a closure enters through) calls it as it called the body:
//
//   entry:   if (cells[k] == 0) goto resolve; else goto call
//   resolve: bronze_cold_resolve(&__bronze_cold_link, k)   // fills cells[k]
//   call:    return cells[k](params...)
//
// The cell is the image's own word, not a thread's: the code it points at,
// the function's entry in the companion's tiered pipeline, runs on any
// thread. Counting the function's entries (TranslatorOptions::
// cold_entry_count) is its body's, in whichever tier runs it.
bool IlLowering::build_interpreted_stub(size_t i, Module& mod) {
    Function* fn = mod.get_function(resolved_names_[i]);
    if (fn == nullptr) return false;
    mod.add_external_symbol("bronze_cold_resolve");
    const uint32_t cell = options_.interpreted[i];
    const auto offset = static_cast<int32_t>(cell * sizeof(uint64_t));
    Builder b(mod);
    b.set_function(fn);
    BasicBlock* entry = b.append_block("entry");
    BasicBlock* resolve = b.append_block("resolve");
    BasicBlock* call = b.append_block("call");
    resolve->set_cold();
    std::vector<Value*> params;
    params.reserve(fn->param_types().size());
    for (const Type& t : fn->param_types()) params.push_back(b.add_block_param(entry, t));

    b.position_at_end(entry);
    const std::string cells_sym = module_sym(std::string(kColdCellsSymbol));
    Value* word = b.build_load(Type::i64(), b.build_func_addr(cells_sym), offset);
    b.build_br_if(b.build_eq(word, b.build_iconst_i64(0)), resolve, call);

    b.position_at_end(resolve);
    b.build_call("bronze_cold_resolve", Type::void_type(),
                 {b.build_func_addr(module_sym(std::string(kColdLinkSymbol))),
                  b.build_iconst_i32(static_cast<int32_t>(cell))});
    b.build_br(call);

    b.position_at_end(call);
    Value* target = b.build_load(Type::ptr(), b.build_func_addr(cells_sym), offset);
    Value* result = b.build_call_indirect(target, fn->return_type(), Span<Value* const>(params.data(), params.size()));
    if (fn->return_type().is_void()) {
        b.build_ret_void();
    } else {
        b.build_ret(result);
    }
    fn->rebuild_cfg_predecessors();
    return true;
}

} // namespace il2mir
