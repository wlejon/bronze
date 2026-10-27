#pragma once

// The host's say over what a program reaches outside itself: one gate every
// native asks before it reads or writes a file, starts a process, opens a
// connection, starts capturing sound, reads the environment or the user's
// desktop configuration; and one ledger of what is held open under such an
// answer, so a host that takes a grant back while the program runs can close
// it.
//
// Natives live in many libraries (a host's siblings, each with its own
// `<name>_api`), and all of them already link this embedding API, so the gate
// is here: one hook a host installs once, asked the same way by all of them.
// With no gate installed every check answers "" (allowed) and nothing changes.
//
// ---- asking ------------------------------------------------------------------
//
//   if (std::string refused = ev::checkAccess(ev::AccessKind::Read, path);
//       !refused.empty())
//       return ev::throwAccessError(refused, "EACCES", path);
//
// The gate answers "" to allow, or the sentence the refusal carries (naming
// the app, what it lacks and what it tried). It is called on the JS thread
// that made the call, synchronously, before any work is queued or any file is
// opened, so it may block to ask the user; it runs on every thread a realm
// runs on (Workers included) and must be thread-safe.
//
// A refusal for something the host granted earlier and has since taken back
// begins with kPermissionRevokedPrefix. newAccessError makes it an Error named
// "PermissionRevoked" (the prefix dropped from its message); every other
// refusal is a plain Error. Both carry `code` and, for a path, `path`.
//
// ---- holding -----------------------------------------------------------------
//
// A resource that stays open under an answer (a microphone capture, a socket,
// a server, a watcher, a child process, a terminal) is registered with
// holdAccess on the thread that owns it, and released when it closes. When the
// host's grants change it bumps the epoch (accessChanged); each thread then
// calls revalidateHeldAccess at its next loop turn, which asks the gate again,
// without prompting, about every resource that thread holds, and calls each
// refused one's `revoke` with the refusal. `revoke` closes the resource and
// reports the refusal to the program (an error event, a rejected promise, an
// 'exit' with the reason): the resource is gone, never left half open.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "embed/embed_handle.h"

namespace bronze {
class Realm;
}

namespace bronze::embed {

using Realm = bronze::Realm;

enum class AccessKind : uint8_t {
    Read,          // subject: an absolute path read (listed, watched, stat'ed, opened)
    Write,         // subject: an absolute path created, changed, renamed or removed
    Spawn,         // subject: the program and its arguments as one line
    Net,           // subject: the endpoint's URL (http(s), ws(s), tcp://, udp://,
                   // tcp-listen://, udp-bind://, ipc://<path>)
    Capture,       // subject: what is recorded ("record from the microphone")
    EnvRead,       // subject: an environment variable's name, "" for all of them
    EnvWrite,      // subject: an environment variable's name
    DesktopRead,   // subject: the user's desktop configuration read ("the recent files")
    DesktopWrite,  // subject: the change to it ("set the default app for text/plain to ...")
};

const char* accessKindName(AccessKind kind);  // "read", "write", "spawn", ...

struct AccessRequest {
    AccessKind kind;
    std::string_view subject;
    // Asked again about a resource already open (revalidateHeldAccess), or
    // through checkAccessWithoutAsking: the gate answers from what is decided
    // and never asks the user.
    bool held = false;
};

using AccessGate = std::function<std::string(const AccessRequest&)>;

/// Install the process-wide gate (an empty function removes it). Safe while
/// realms run: calls already inside the old gate finish against it.
BRONZE_EMBED_API void setAccessGate(AccessGate gate);

/// True when a gate is installed: a caller skips building an argument (an
/// absolute path, a command line) when there is nothing to ask.
BRONZE_EMBED_API bool accessGateActive();

/// The gate's answer: "" when allowed or when no gate is installed.
BRONZE_EMBED_API std::string checkAccess(AccessKind kind, std::string_view subject);

/// The same, answered from what is decided without asking the user (the
/// request's `held` set): for a question that would otherwise be asked many
/// times over, such as each variable of a listing already refused as a whole.
BRONZE_EMBED_API std::string checkAccessWithoutAsking(AccessKind kind, std::string_view subject);

inline constexpr std::string_view kPermissionRevokedPrefix = "PermissionRevoked: ";

/// Whether `refusal` is for a grant taken back while the program ran.
BRONZE_EMBED_API bool isRevokedRefusal(std::string_view refusal);

/// The refusal's sentence without the revoked prefix.
BRONZE_EMBED_API std::string refusalMessage(std::string_view refusal);

/// The refusal as an Error (name "PermissionRevoked" for a revoked grant) with
/// `code` and, when not empty, `path`. Fresh: root it before allocating.
BRONZE_EMBED_API Value newAccessError(std::string_view refusal, std::string_view code = "EACCES",
                                      std::string_view path = {});

/// Throw newAccessError into the running program; returns what the NativeFn
/// should return.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4646)  // noreturn with a non-void type, on purpose
#endif
[[noreturn]] BRONZE_EMBED_API Value throwAccessError(std::string_view refusal, std::string_view code = "EACCES",
                                                     std::string_view path = {});
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/// A promise already rejected with newAccessError.
BRONZE_EMBED_API Value rejectedAccessPromise(std::string_view refusal, std::string_view code = "EACCES",
                                             std::string_view path = {});

// ---- the held-access ledger ---------------------------------------------------

using HeldAccessId = uint64_t;

/// Register a resource open under (kind, subject) on the calling thread.
/// `revoke` runs on that thread, from revalidateHeldAccess, with the refusal;
/// the entry is released before it runs, so it may close the resource through
/// the same path that calls releaseAccess (a second release is a no-op).
BRONZE_EMBED_API HeldAccessId holdAccess(AccessKind kind, std::string subject,
                                         std::function<void(const std::string& refusal)> revoke);

/// The resource closed: forget it. 0 and unknown ids are ignored.
BRONZE_EMBED_API void releaseAccess(HeldAccessId id);
/// A realm is going away (the host stops the app it ran): revoke everything
/// it holds open on the calling thread with `refusal`, inside the realm, and
/// return how many. Each entry remembers the realm current at holdAccess, and
/// revalidateHeldAccess asks the gate about it inside that realm too.
BRONZE_EMBED_API int revokeHeldAccessInRealm(Realm* realm, const std::string& refusal);

/// The host's grants changed: every thread revalidates at its next turn.
BRONZE_EMBED_API void accessChanged();

/// Ask the gate again about everything the calling thread holds, when the
/// epoch moved since this thread last did (or `force`), and revoke what it
/// refuses. Returns how many were revoked. Cheap when nothing changed (one
/// atomic load), so a loop calls it every turn.
BRONZE_EMBED_API int revalidateHeldAccess(bool force = false);

/// How many resources the calling thread holds open (for tests).
BRONZE_EMBED_API size_t heldAccessCount();

}  // namespace bronze::embed
