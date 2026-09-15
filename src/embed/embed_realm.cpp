#include "embed/embed.h"
#include "runtime/gc.h"
#include "runtime/realm.h"

namespace bronze::embed {

Realm* createRealm(Value customGlobal) {
    ShadowStackFrame frame;
    return bronze::rtCreateRealm(customGlobal);
}

void destroyRealm(Realm* realm) {
    ShadowStackFrame frame;
    bronze::rtDestroyRealm(realm);
}

void enterRealm(Realm* realm) {
    ShadowStackFrame frame;
    bronze::rtEnterRealm(realm);
}

void exitRealm() {
    ShadowStackFrame frame;
    bronze::rtExitRealm();
}

Value getRealmGlobal(Realm* realm) {
    ShadowStackFrame frame;
    if (!realm) {
        realm = bronze::rtCurrentRealm();
    }
    return realm ? realm->globalObject() : Value::fromUndefined();
}

Realm* currentRealm() {
    ShadowStackFrame frame;
    return bronze::rtCurrentRealm();
}

}  // namespace bronze::embed
