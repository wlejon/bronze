// The access gate and the held-access ledger (embed_access.h).
//
// The gate is set before realms run, but every realm's thread reads it, and a
// host may replace it while they do. So it lives behind a shared_ptr swapped
// under a mutex: a check copies the pointer under the lock and calls the gate
// outside it, so a gate that blocks to ask the user holds no lock, and a
// replaced gate stays alive until the calls already in it return. The flag
// spares the common case, no gate at all, the lock.
//
// The ledger is process-wide, keyed by id, each entry naming the thread that
// owns it: only that thread revalidates or revokes it, since what `revoke`
// closes (and the Persistents it captures) belongs to that thread's runtime.
// It is never freed: entries a thread still holds at exit would otherwise be
// destroyed after the runtimes they point into.

#include "embed/embed_access.h"

#include "embed/embed.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bronze::embed {

namespace {

std::mutex g_gateMu;
std::shared_ptr<const AccessGate> g_gate;
std::atomic<bool> g_gateActive{false};

std::shared_ptr<const AccessGate> currentGate() {
    if (!g_gateActive.load(std::memory_order_acquire)) return nullptr;
    std::lock_guard lock(g_gateMu);
    return g_gate;
}

struct HeldEntry {
    std::thread::id owner;
    AccessKind kind;
    std::string subject;
    std::function<void(const std::string&)> revoke;
};

struct Ledger {
    std::mutex mu;
    std::unordered_map<HeldAccessId, HeldEntry> entries;
    HeldAccessId nextId = 1;
};

Ledger& ledger() {
    static Ledger* l = new Ledger();
    return *l;
}

std::atomic<uint64_t> g_epoch{1};
thread_local uint64_t t_seenEpoch = 1;

}  // namespace

const char* accessKindName(AccessKind kind) {
    switch (kind) {
        case AccessKind::Read: return "read";
        case AccessKind::Write: return "write";
        case AccessKind::Spawn: return "spawn";
        case AccessKind::Net: return "net";
        case AccessKind::Capture: return "capture";
        case AccessKind::EnvRead: return "env-read";
        case AccessKind::EnvWrite: return "env-write";
        case AccessKind::DesktopRead: return "desktop-read";
        case AccessKind::DesktopWrite: return "desktop-write";
    }
    return "?";
}

void setAccessGate(AccessGate gate) {
    const bool any = static_cast<bool>(gate);
    auto next = any ? std::make_shared<const AccessGate>(std::move(gate)) : nullptr;
    std::lock_guard lock(g_gateMu);
    g_gate = std::move(next);
    g_gateActive.store(any, std::memory_order_release);
}

bool accessGateActive() { return g_gateActive.load(std::memory_order_acquire); }

namespace {

std::string ask(AccessKind kind, std::string_view subject, bool held) {
    auto gate = currentGate();
    if (!gate) return {};
    return (*gate)(AccessRequest{kind, subject, held});
}

}  // namespace

std::string checkAccess(AccessKind kind, std::string_view subject) { return ask(kind, subject, false); }

bool isRevokedRefusal(std::string_view refusal) {
    return refusal.substr(0, kPermissionRevokedPrefix.size()) == kPermissionRevokedPrefix;
}

std::string refusalMessage(std::string_view refusal) {
    if (isRevokedRefusal(refusal)) refusal.remove_prefix(kPermissionRevokedPrefix.size());
    return std::string(refusal);
}

Value newAccessError(std::string_view refusal, std::string_view code, std::string_view path) {
    Persistent msg{fromUtf8(refusalMessage(refusal))};
    Persistent err{msg.get()};
    {
        GlobalValue g = globalValue("Error");
        if (g.found && isFunction(g.value)) {
            Persistent ctor{g.value};
            const Value args[1] = {msg.get()};
            CallResult r = construct(ctor.get(), std::span<const Value>(args, 1));
            if (!r.thrown) err.set(r.value);
        }
    }
    if (!isObject(err.get())) return err.get();
    // Each value is made in a statement of its own: built inside the call, it
    // could move the receiver after err.get() was read.
    if (isRevokedRefusal(refusal)) {
        Persistent name{fromUtf8("PermissionRevoked")};
        err.set(setProperty(err.get(), "name", name.get()));
    }
    if (!code.empty()) {
        Persistent codeVal{fromUtf8(code)};
        err.set(setProperty(err.get(), "code", codeVal.get()));
    }
    if (!path.empty()) {
        Persistent pathVal{fromUtf8(path)};
        err.set(setProperty(err.get(), "path", pathVal.get()));
    }
    return err.get();
}

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4646)  // embed_access.h: noreturn with a non-void type
#endif
Value throwAccessError(std::string_view refusal, std::string_view code, std::string_view path) {
    throwValue(newAccessError(refusal, code, path));
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

Value rejectedAccessPromise(std::string_view refusal, std::string_view code, std::string_view path) {
    Persistent promise{createPromise()};
    Persistent err{newAccessError(refusal, code, path)};
    rejectPromise(promise.get(), err.get());
    return promise.get();
}

HeldAccessId holdAccess(AccessKind kind, std::string subject, std::function<void(const std::string&)> revoke) {
    Ledger& l = ledger();
    std::lock_guard lock(l.mu);
    const HeldAccessId id = l.nextId++;
    l.entries.emplace(id, HeldEntry{std::this_thread::get_id(), kind, std::move(subject), std::move(revoke)});
    return id;
}

void releaseAccess(HeldAccessId id) {
    if (id == 0) return;
    std::function<void(const std::string&)> dropped;
    {
        Ledger& l = ledger();
        std::lock_guard lock(l.mu);
        auto it = l.entries.find(id);
        if (it == l.entries.end()) return;
        dropped = std::move(it->second.revoke);
        l.entries.erase(it);
    }
    // `dropped` (and what it captured) is destroyed here, outside the lock.
}

void accessChanged() { g_epoch.fetch_add(1, std::memory_order_acq_rel); }

int revalidateHeldAccess(bool force) {
    const uint64_t epoch = g_epoch.load(std::memory_order_acquire);
    if (!force && epoch == t_seenEpoch) return 0;
    t_seenEpoch = epoch;
    const std::thread::id self = std::this_thread::get_id();
    struct Probe {
        HeldAccessId id;
        AccessKind kind;
        std::string subject;
    };
    std::vector<Probe> mine;
    {
        Ledger& l = ledger();
        std::lock_guard lock(l.mu);
        for (const auto& [id, e] : l.entries)
            if (e.owner == self) mine.push_back({id, e.kind, e.subject});
    }
    int revoked = 0;
    for (const Probe& p : mine) {
        std::string refused = ask(p.kind, p.subject, /*held=*/true);
        if (refused.empty()) continue;
        std::function<void(const std::string&)> revoke;
        {
            Ledger& l = ledger();
            std::lock_guard lock(l.mu);
            auto it = l.entries.find(p.id);
            if (it == l.entries.end()) continue;  // closed by an earlier revoke
            revoke = std::move(it->second.revoke);
            l.entries.erase(it);
        }
        ++revoked;
        if (revoke) revoke(refused);
    }
    return revoked;
}

size_t heldAccessCount() {
    const std::thread::id self = std::this_thread::get_id();
    Ledger& l = ledger();
    std::lock_guard lock(l.mu);
    size_t n = 0;
    for (const auto& [id, e] : l.entries) n += e.owner == self;
    return n;
}

}  // namespace bronze::embed
