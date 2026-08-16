// A host that links TWO compiled bronze modules against one runtime and enters
// them in turn — the thing one `bronze_main` per image made impossible.
//
// What each piece is proving:
//
//  * Two objects, two entry symbols. `--entry-symbol` names the entry, and the
//    object's ABI stamp is named after it, so the only two symbols a compiled
//    object exports are distinct per module. Everything else it defines is
//    internal, which is what makes the link work at all.
//
//  * One key registry. Each module numbers its own key strings 0..n-1; at init
//    the runtime INTERNS them and the module records the process-wide ids in
//    its own remap. `registry.count` written by B and read by A's `bump()` is
//    one property only because "count" interned to one id in both.
//
//  * Per-module bookkeeping. The global cache, the function-singleton slots and
//    the module-environment cell are arrays in each object's own data,
//    registered here as GC root spans at module init. A collection between the
//    two entries — and, under BRONZE_GC_STRESS, at every allocation inside them
//    — is what makes those spans load-bearing rather than decorative.
//
//  * One exception cell. B throws; the `try` compiled into A catches.
//
// Deliberately NOT `embed::runMain()`: that names `bronze_main`, and a host
// with two modules has no function by that name. It open-codes the same
// sequence — ABI check, root frame, entry, microtask checkpoint — twice.

#include <cstdint>
#include <cstdio>
#include <string>

#include "embed/embed.h"
#include "runtime/abi_guard.h"
#include "runtime/gc.h"
#include "runtime/microtask.h"
#include "runtime/rt_state.h"

extern "C" void bronze_module_a();
extern "C" void bronze_module_b();
extern "C" const uint32_t bronze_module_a_abi_fingerprint;
extern "C" const uint32_t bronze_module_b_abi_fingerprint;

namespace {

void collect() { bronze::runtime::rtHeap().collect(); }

// The global object, through the same two calls generated code makes: intern
// the name, then ask for the global. A second interning of a string a module
// already registered must answer that module's id, which is the property the
// whole scheme rests on — so the host asking for "globalThis" here is asking
// through exactly the mechanism under test.
bronze::Value globalObject() {
    const uint32_t key = bronze_register_key_string("globalThis");
    // No cache cell: the host is not a module and has no table to fill.
    return bronze::Value(bronze_global_get(key, nullptr));
}

// Calls a zero-argument function hanging off globalThis and prints its result.
void printCallResult(const char* what, const char* name) {
    bronze::Value fn = bronze::embed::getProperty(globalObject(), name);
    bronze::embed::CallResult r = bronze::embed::call(fn, bronze::embed::undefined(), {});
    if (r.thrown) {
        std::printf("host %s THREW\n", what);
        return;
    }
    std::printf("host %s=%s\n", what, bronze::embed::toUtf8(r.value).c_str());
}

}  // namespace

int main() {
    bronze::embed::setupIo();

    // Each object carries its own stamp under its own name, and both are
    // checked before either module runs — the same guard runMain performs, once
    // per module rather than once per image.
    bronze::runtime::rtCheckObjectAbi(bronze_module_a_abi_fingerprint);
    bronze::runtime::rtCheckObjectAbi(bronze_module_b_abi_fingerprint);

    bronze::ShadowStackFrame root_frame;

    bronze_module_a();
    bronze::runtime::rtDrainMicrotasks();

    // Between the two entries: everything A built moves, including whatever A's
    // own cells point at, before B has run a single instruction.
    collect();

    bronze_module_b();
    bronze::runtime::rtDrainMicrotasks();

    // And after both, before the host reads anything back.
    collect();

    // A's closure over A's module scope, answering from the host after two
    // collections and after B wrote to it through A's setter.
    printCallResult("secret", "readSecret");

    // A's `bump` once more, from a third caller that is neither module: the
    // count has to continue from where B left it.
    bronze::Value bump = bronze::embed::getProperty(globalObject(), "bump");
    const bronze::Value one = bronze::embed::fromDouble(1.0);
    bronze::embed::CallResult bumped =
        bronze::embed::call(bump, bronze::embed::undefined(), {&one, 1});
    std::printf("host bump=%g\n", bumped.value.asNumber());

    collect();
    bronze::Value items =
        bronze::embed::getProperty(bronze::embed::getProperty(globalObject(), "registry"),
                                   "items");
    bronze::Value joiner = bronze::embed::getProperty(items, "length");
    std::printf("host items.length=%g\n", joiner.asNumber());
    return 0;
}
