#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace bronze {

class Realm {
public:
    explicit Realm(bool deferInit = false);
    explicit Realm(Value customGlobal);
    ~Realm() = default;

    Realm(const Realm&) = delete;
    Realm& operator=(const Realm&) = delete;

    // The realm's globalThis object.
    Value globalObject() const noexcept { return globalObject_; }
    void setGlobalObject(Value glob) noexcept { globalObject_ = glob; }

    // Optional environment cell associated with this realm.
    Value globalEnv() const noexcept { return globalEnv_; }
    void setGlobalEnv(Value env) noexcept { globalEnv_ = env; }

    // GC root tracing for this realm's held heap values.
    void visitRoots(const Heap::RootVisitor& visit);

    // Populate the global object with builtins, host globals, and globalThis reference.
    void initGlobalObject(Value customGlobal = Value::fromUndefined());

private:
    Value globalObject_{Value::fromUndefined()};
    Value globalEnv_{Value::fromUndefined()};
};

// Thread-local active Realm accessors and stack operations.
Realm* rtCurrentRealm();
Realm* rtDefaultRealm();
Realm* rtCreateRealm(Value customGlobal = Value::fromUndefined());
void rtDestroyRealm(Realm* realm);
void rtEnterRealm(Realm* realm);
void rtExitRealm();

// Trace all live realms on the calling thread during GC.
void rtVisitRealmRoots(const Heap::RootVisitor& visit);

}  // namespace bronze
