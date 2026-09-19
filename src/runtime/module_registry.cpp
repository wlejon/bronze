// The realm's module registry. module_registry.h states what it is for; this
// file is the storage, the two intrinsics the linker's generated source calls,
// and nothing else.

#include "runtime/module_registry.h"

#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/gc.h"
#include "runtime/realm.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// A path argument as text. The specifier keys the compiler writes are ASCII
// file paths it produced itself, so this is a read and never a coercion that
// could run user code — but it goes through the ordinary string check anyway,
// because the intrinsic is a global and a program can reach it.
bool pathArgument(Value v, std::string& out) {
    if (!v.isString()) return false;
    out = rtUtf8Chars(v.asString<StringHeader>());
    return true;
}

uint64_t modulePublish(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    std::string path;
    if (!pathArgument(args[0], path)) {
        return rtThrowTypeError("__bronze_module_publish: the first argument must be a string")
            .rawBits();
    }
    rtModulePublish(path, args[1]);
    return Value::fromUndefined().rawBits();
}

uint64_t moduleLookup(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    std::string path;
    if (!pathArgument(args[0], path)) {
        return rtThrowTypeError("__bronze_module_lookup: the first argument must be a string")
            .rawBits();
    }
    Value found;
    if (!rtModuleLookup(path, found)) {
        // The compiler only emits this call for a path it SAW in the registry,
        // so a miss means the registry changed between the compile and the run
        // — a realm was destroyed, or an app reloaded mid-compile. Thrown
        // rather than fatal: the host can report it and carry on.
        return rtThrowTypeError("__bronze_module_lookup: no module instance published for " + path)
            .rawBits();
    }
    return found.rawBits();
}

}  // namespace

void rtModulePublish(const std::string& path, Value ns) {
    Realm* realm = rtCurrentRealm();
    if (!realm) return;
    for (auto& entry : realm->moduleRegistry()) {
        if (entry.first == path) {
            entry.second = ns;
            return;
        }
    }
    realm->moduleRegistry().emplace_back(path, ns);
}

bool rtModuleLookup(const std::string& path, Value& out) {
    Realm* realm = rtCurrentRealm();
    if (!realm) return false;
    for (const auto& entry : realm->moduleRegistry()) {
        if (entry.first == path) {
            out = entry.second;
            return true;
        }
    }
    return false;
}

std::vector<std::string> rtModuleRegistryPaths() {
    std::vector<std::string> out;
    Realm* realm = rtCurrentRealm();
    if (!realm) return out;
    out.reserve(realm->moduleRegistry().size());
    for (const auto& entry : realm->moduleRegistry()) out.push_back(entry.first);
    return out;
}

Value rtModuleRegistryIntrinsic(const std::string& name) {
    if (name == "__bronze_module_publish") {
        return rtNativeFunction(modulePublish, 2, "__bronze_module_publish", 2);
    }
    if (name == "__bronze_module_lookup") {
        return rtNativeFunction(moduleLookup, 1, "__bronze_module_lookup", 1);
    }
    return Value::fromUndefined();
}

}  // namespace bronze::runtime
