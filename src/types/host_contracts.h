#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "ast/ast.h"
#include "types/type.h"

namespace bronze::types {

// Nullability contract for a host, DOM, or Web API call or property read.
enum class HostNullability : uint8_t {
    Unknown,
    DefinitelyNonNullish,
    MaybeNullish,
    AlwaysNullish,
};

// Known structured object kinds returned by host/DOM APIs whose numeric and
// string properties have fixed, specification-guaranteed types.
enum class HostObjectKind : uint8_t {
    None = 0,
    DOMRect,
    DOMEvent,
    DOMElement,
    BroDisplay,
    BroSysInfo,
};

using UserBindingPredicate = std::function<bool(const std::string&)>;
using HostObjectLookup = std::function<HostObjectKind(const std::string&)>;

// True when `<baseGlobal>.<prop>` is a host/builtin global property that
// always evaluates to a JS Number on an unshadowed global (`window.innerWidth`,
// `window.devicePixelRatio`, `screen.width`, `performance.timeOrigin`,
// `Number.MAX_SAFE_INTEGER`, etc.).
bool isHostGlobalNumericProperty(std::string_view baseGlobal, std::string_view prop);

// True when `bro.<subNamespace>.<prop>` is a Bronze native namespace property
// that always evaluates to a JS Number (`bro.sys.uptime`, `bro.compositor.fps`,
// `bro.displays.primaryScale`, `bro.audio.masterVolume`, etc.).
bool isBroNamespaceNumericProperty(std::string_view subNamespace, std::string_view prop);

// True when `call` is an unshadowed host/DOM/builtin call that always returns
// a JS Number (`performance.now()`, `Date.now()`, `Number(x)`, `parseFloat(x)`,
// `parseInt(x)`, `requestAnimationFrame(cb)`, `bro.sys.now()`, etc.).
bool isHostCallReturningNumber(const ast::Call& call, const UserBindingPredicate& isUserBound,
                               bool mathPristine = true);

// Classifies the structured host object kind returned by `call`
// (`el.getBoundingClientRect()` -> `DOMRect`, `document.createElement(...)` ->
// `DOMElement`, `bro.sys.info()` -> `BroSysInfo`, etc.).
HostObjectKind classifyHostCallReturnedObject(const ast::Call& call,
                                              const UserBindingPredicate& isUserBound);

// Returns the guaranteed property type on a known `HostObjectKind`, or
// `Type::dynamic()` if the property is not covered by a host contract.
Type hostObjectPropertyType(HostObjectKind kind, std::string_view prop);

// Returns the guaranteed type of member read `m` when it matches a host/DOM
// global property, `bro.*` namespace property, chained host object call
// (`el.getBoundingClientRect().width`), or tracked local `HostObjectKind`
// binding (`const rect = el.getBoundingClientRect(); rect.width`).
Type classifyHostMemberReadType(const ast::MemberAccess& m,
                                const UserBindingPredicate& isUserBound,
                                const HostObjectLookup& lookupHostObj,
                                bool mathPristine = true);

// Classifies the nullability of a host/DOM API call (`document.getElementById`,
// `document.querySelector`, `el.closest`, `localStorage.getItem`,
// `bro.displays.byId`, etc.).
HostNullability classifyHostCallNullability(const ast::Call& call,
                                            const UserBindingPredicate& isUserBound);

// Classifies the nullability of a host/DOM property read
// (`document.activeElement`, `el.parentElement`, `el.firstElementChild`,
// `el.nextElementSibling`, `document.body`, etc.).
HostNullability classifyHostPropertyNullability(const ast::MemberAccess& member,
                                                const UserBindingPredicate& isUserBound);

// Produces a concise human-readable description of a nullable host call or
// property expression for static checker diagnostics.
std::string describeHostNullabilitySource(const ast::Expr& expr);

}  // namespace bronze::types
