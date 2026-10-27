#include "embed/embed.h"
#include <utility>

#include "runtime/gc.h"
#include "runtime/locale_hooks.h"
#include "runtime/lockdown.h"
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

void joinRealmGroup(Realm* realm, Realm* member) {
    if (!realm || !member || realm == member) return;
    realm->setGroup(member->group());
}

bool sameRealmGroup(const Realm* a, const Realm* b) {
    return a != nullptr && b != nullptr && a->group() == b->group();
}

void addRealmDestroyHook(RealmDestroyHook hook) { bronze::rtAddRealmDestroyHook(hook); }

void lockdown() {
    ShadowStackFrame frame;
    runtime::rtLockdown();
}

bool lockedDown() { return runtime::rtLockedDown(); }

void setLocaleMethod(LocaleMethod which, NativeFn fn) {
    using runtime::LocaleHook;
    LocaleHook hook = LocaleHook::Count;
    switch (which) {
        case LocaleMethod::NumberToLocaleString: hook = LocaleHook::NumberToLocaleString; break;
        case LocaleMethod::DateToLocaleString: hook = LocaleHook::DateToLocaleString; break;
        case LocaleMethod::DateToLocaleDateString: hook = LocaleHook::DateToLocaleDateString; break;
        case LocaleMethod::DateToLocaleTimeString: hook = LocaleHook::DateToLocaleTimeString; break;
        case LocaleMethod::StringLocaleCompare: hook = LocaleHook::StringLocaleCompare; break;
    }
    runtime::rtSetLocaleHook(hook, std::move(fn));
}

}  // namespace bronze::embed
