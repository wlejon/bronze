// The access gate and the held-access ledger (src/embed/embed_access.h).

#include <doctest/doctest.h>

#include <string>
#include <thread>
#include <vector>

#include "embed/embed.h"
#include "embed/embed_access.h"

using namespace bronze;
namespace ev = bronze::embed;

namespace {

// Clears the gate however a case ends.
struct GateGuard {
    ~GateGuard() { ev::setAccessGate(nullptr); }
};

std::string readProp(Value obj, const char* key) {
    ev::Persistent o{obj};
    Value v = ev::getProperty(o.get(), key);
    return ev::isString(v) ? ev::toUtf8(v) : std::string("<not a string>");
}

}  // namespace

TEST_CASE("access: no gate allows everything") {
    ev::setAccessGate(nullptr);
    CHECK_FALSE(ev::accessGateActive());
    CHECK(ev::checkAccess(ev::AccessKind::Read, "/etc/passwd").empty());
    CHECK(ev::checkAccess(ev::AccessKind::Spawn, "sh -c true").empty());
}

TEST_CASE("access: the gate sees each kind and subject, and its answer is the check's") {
    GateGuard guard;
    std::vector<std::string> seen;
    ev::setAccessGate([&](const ev::AccessRequest& r) -> std::string {
        seen.push_back(std::string(ev::accessKindName(r.kind)) + ":" + std::string(r.subject));
        if (r.kind == ev::AccessKind::Write) return "no writing " + std::string(r.subject);
        return {};
    });
    CHECK(ev::accessGateActive());
    CHECK(ev::checkAccess(ev::AccessKind::Read, "/a").empty());
    CHECK(ev::checkAccess(ev::AccessKind::Write, "/b") == "no writing /b");
    CHECK(ev::checkAccess(ev::AccessKind::EnvRead, "HOME").empty());
    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == "read:/a");
    CHECK(seen[1] == "write:/b");
    CHECK(seen[2] == "env-read:HOME");
}

TEST_CASE("access: refusals become Errors, a revoked one named PermissionRevoked") {
    {
        ev::Persistent e{ev::newAccessError("App may not read /x", "EACCES", "/x")};
        CHECK(readProp(e.get(), "message") == "App may not read /x");
        CHECK(readProp(e.get(), "code") == "EACCES");
        CHECK(readProp(e.get(), "path") == "/x");
        CHECK(readProp(e.get(), "name") == "Error");
    }
    {
        const std::string refusal = std::string(ev::kPermissionRevokedPrefix) + "App may no longer use the network";
        CHECK(ev::isRevokedRefusal(refusal));
        CHECK(ev::refusalMessage(refusal) == "App may no longer use the network");
        ev::Persistent e{ev::newAccessError(refusal, "ERR_ACCESS_DENIED")};
        CHECK(readProp(e.get(), "name") == "PermissionRevoked");
        CHECK(readProp(e.get(), "message") == "App may no longer use the network");
        CHECK(readProp(e.get(), "code") == "ERR_ACCESS_DENIED");
    }
}

TEST_CASE("access: held resources are revoked on their own thread when the gate refuses them") {
    GateGuard guard;
    bool netAllowed = true;
    ev::setAccessGate([&](const ev::AccessRequest& r) -> std::string {
        if (r.kind == ev::AccessKind::Net && !netAllowed) {
            CHECK(r.held);  // a revalidation never prompts
            return std::string(ev::kPermissionRevokedPrefix) + "no network";
        }
        return {};
    });
    const size_t before = ev::heldAccessCount();
    std::vector<std::string> closed;
    const ev::HeldAccessId sock =
        ev::holdAccess(ev::AccessKind::Net, "tcp://127.0.0.1:9", [&](const std::string& why) { closed.push_back("sock:" + why); });
    const ev::HeldAccessId watcher =
        ev::holdAccess(ev::AccessKind::Read, "/tmp", [&](const std::string& why) { closed.push_back("watch:" + why); });
    CHECK(ev::heldAccessCount() == before + 2);

    // Nothing changed: no revalidation, nothing asked.
    ev::revalidateHeldAccess();
    CHECK(closed.empty());

    // Another thread's revalidation does not touch this thread's resources.
    netAllowed = false;
    ev::accessChanged();
    std::thread other([] { CHECK(ev::revalidateHeldAccess() == 0); });
    other.join();
    CHECK(closed.empty());

    CHECK(ev::revalidateHeldAccess() == 1);
    REQUIRE(closed.size() == 1);
    CHECK(closed[0] == std::string("sock:") + std::string(ev::kPermissionRevokedPrefix) + "no network");
    CHECK(ev::heldAccessCount() == before + 1);

    // Revoked once: a second pass (or a late release) finds nothing.
    ev::releaseAccess(sock);
    CHECK(ev::revalidateHeldAccess(/*force=*/true) == 0);
    ev::releaseAccess(watcher);
    CHECK(ev::heldAccessCount() == before);
}
