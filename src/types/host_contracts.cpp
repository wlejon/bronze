#include "types/host_contracts.h"

#include "ast/queries.h"
#include "types/math_builtins.h"

namespace bronze::types {

bool isHostGlobalNumericProperty(std::string_view baseGlobal, std::string_view prop) {
    if (baseGlobal == "window" || baseGlobal == "globalThis" || baseGlobal == "self") {
        return prop == "innerWidth" || prop == "innerHeight" || prop == "outerWidth" ||
               prop == "outerHeight" || prop == "devicePixelRatio" || prop == "scrollX" ||
               prop == "scrollY" || prop == "pageXOffset" || prop == "pageYOffset" ||
               prop == "screenX" || prop == "screenY";
    }
    if (baseGlobal == "screen") {
        return prop == "width" || prop == "height" || prop == "availWidth" ||
               prop == "availHeight" || prop == "colorDepth" || prop == "pixelDepth";
    }
    if (baseGlobal == "performance") {
        return prop == "timeOrigin";
    }
    if (baseGlobal == "Number") {
        return prop == "EPSILON" || prop == "MAX_SAFE_INTEGER" || prop == "MIN_SAFE_INTEGER" ||
               prop == "MAX_VALUE" || prop == "MIN_VALUE" || prop == "NaN" ||
               prop == "POSITIVE_INFINITY" || prop == "NEGATIVE_INFINITY";
    }
    return false;
}

bool isBroNamespaceNumericProperty(std::string_view subNamespace, std::string_view prop) {
    if (subNamespace == "sys") {
        return prop == "uptime" || prop == "cores" || prop == "totalMemory" ||
               prop == "freeMemory";
    }
    if (subNamespace == "audio") {
        return prop == "masterVolume" || prop == "sampleRate";
    }
    if (subNamespace == "compositor") {
        return prop == "fps" || prop == "frameCount" || prop == "surfaceCount" ||
               prop == "width" || prop == "height";
    }
    if (subNamespace == "displays") {
        return prop == "count" || prop == "primaryWidth" || prop == "primaryHeight" ||
               prop == "primaryScale";
    }
    return false;
}

bool isHostCallReturningNumber(const ast::Call& call, const UserBindingPredicate& isUserBound,
                               bool mathPristine) {
    if (call.optional) return false;
    for (const auto& arg : call.args) {
        if (ast::exactAs<const ast::SpreadElement*>(arg.get()) != nullptr) return false;
    }

    if (const auto* calleeId = ast::exactAs<const ast::Ident*>(call.callee.get())) {
        if (isUserBound && isUserBound(calleeId->name)) return false;
        if (calleeId->name == "Number" && call.args.size() <= 1) return true;
        if (calleeId->name == "parseFloat" && !call.args.empty()) return true;
        if (calleeId->name == "parseInt" && !call.args.empty()) return true;
        if (calleeId->name == "requestAnimationFrame" && !call.args.empty()) return true;
        return false;
    }

    const auto* member = ast::exactAs<const ast::MemberAccess*>(call.callee.get());
    if (member == nullptr || member->optional || member->isPrivate) return false;

    if (const auto* baseId = ast::exactAs<const ast::Ident*>(member->object.get())) {
        if (isUserBound && isUserBound(baseId->name)) return false;
        if (baseId->name == "Math" && mathPristine) {
            return isMathMethodReturningNumber(member->property);
        }
        if (baseId->name == "performance" && member->property == "now" && call.args.empty()) {
            return true;
        }
        if (baseId->name == "Date" && (member->property == "now" || member->property == "parse" ||
                                       member->property == "UTC")) {
            return true;
        }
        if (baseId->name == "Number" &&
            (member->property == "parseFloat" || member->property == "parseInt") &&
            !call.args.empty()) {
            return true;
        }
        if ((baseId->name == "window" || baseId->name == "globalThis" || baseId->name == "self") &&
            member->property == "requestAnimationFrame" && !call.args.empty()) {
            return true;
        }
        return false;
    }

    if (const auto* outerMem = ast::exactAs<const ast::MemberAccess*>(member->object.get())) {
        if (outerMem->optional || outerMem->isPrivate) return false;
        const auto* rootId = ast::exactAs<const ast::Ident*>(outerMem->object.get());
        if (rootId == nullptr) return false;
        if (isUserBound && isUserBound(rootId->name)) return false;
        if (rootId->name == "bro") {
            if (outerMem->property == "sys" &&
                (member->property == "now" || member->property == "uptimeMs") &&
                call.args.empty()) {
                return true;
            }
            if (outerMem->property == "audio" && member->property == "getVolume" &&
                call.args.empty()) {
                return true;
            }
            if (outerMem->property == "displays" && member->property == "count" &&
                call.args.empty()) {
                return true;
            }
        }
        if ((rootId->name == "window" || rootId->name == "globalThis") &&
            outerMem->property == "performance" && member->property == "now" &&
            call.args.empty()) {
            return true;
        }
    }

    return false;
}

HostObjectKind classifyHostCallReturnedObject(const ast::Call& call,
                                              const UserBindingPredicate& isUserBound) {
    if (call.optional) return HostObjectKind::None;
    const auto* member = ast::exactAs<const ast::MemberAccess*>(call.callee.get());
    if (member == nullptr || member->optional || member->isPrivate) return HostObjectKind::None;

    if (member->property == "getBoundingClientRect" && call.args.empty()) {
        return HostObjectKind::DOMRect;
    }

    if (const auto* baseId = ast::exactAs<const ast::Ident*>(member->object.get())) {
        if (isUserBound && isUserBound(baseId->name)) return HostObjectKind::None;
        if (baseId->name == "document" &&
            (member->property == "createElement" || member->property == "createElementNS") &&
            !call.args.empty()) {
            return HostObjectKind::DOMElement;
        }
        return HostObjectKind::None;
    }

    if (const auto* outerMem = ast::exactAs<const ast::MemberAccess*>(member->object.get())) {
        if (outerMem->optional || outerMem->isPrivate) return HostObjectKind::None;
        const auto* rootId = ast::exactAs<const ast::Ident*>(outerMem->object.get());
        if (rootId == nullptr || (isUserBound && isUserBound(rootId->name))) {
            return HostObjectKind::None;
        }
        if (rootId->name == "bro") {
            if (outerMem->property == "sys" &&
                (member->property == "info" || member->property == "memoryInfo" ||
                 member->property == "cpuInfo") &&
                call.args.empty()) {
                return HostObjectKind::BroSysInfo;
            }
            if (outerMem->property == "displays" && member->property == "primary" &&
                call.args.empty()) {
                return HostObjectKind::BroDisplay;
            }
        }
    }

    return HostObjectKind::None;
}

Type hostObjectPropertyType(HostObjectKind kind, std::string_view prop) {
    switch (kind) {
        case HostObjectKind::None:
            return Type::dynamic();
        case HostObjectKind::DOMRect:
            if (prop == "x" || prop == "y" || prop == "width" || prop == "height" ||
                prop == "top" || prop == "left" || prop == "right" || prop == "bottom") {
                return Type::number();
            }
            return Type::dynamic();
        case HostObjectKind::DOMEvent:
            if (prop == "clientX" || prop == "clientY" || prop == "pageX" || prop == "pageY" ||
                prop == "offsetX" || prop == "offsetY" || prop == "screenX" || prop == "screenY" ||
                prop == "movementX" || prop == "movementY" || prop == "deltaX" ||
                prop == "deltaY" || prop == "deltaZ" || prop == "deltaMode" || prop == "button" ||
                prop == "buttons" || prop == "keyCode" || prop == "which" ||
                prop == "timeStamp" || prop == "detail") {
                return Type::number();
            }
            if (prop == "type" || prop == "key" || prop == "code") {
                return Type::string();
            }
            if (prop == "altKey" || prop == "ctrlKey" || prop == "metaKey" || prop == "shiftKey" ||
                prop == "repeat" || prop == "bubbles" || prop == "cancelable" ||
                prop == "defaultPrevented" || prop == "isTrusted") {
                return Type::boolean();
            }
            return Type::dynamic();
        case HostObjectKind::DOMElement:
            if (prop == "clientWidth" || prop == "clientHeight" || prop == "clientTop" ||
                prop == "clientLeft" || prop == "offsetWidth" || prop == "offsetHeight" ||
                prop == "offsetTop" || prop == "offsetLeft" || prop == "scrollWidth" ||
                prop == "scrollHeight" || prop == "scrollTop" || prop == "scrollLeft" ||
                prop == "tabIndex") {
                return Type::number();
            }
            if (prop == "tagName" || prop == "id" || prop == "className" || prop == "innerHTML" ||
                prop == "outerHTML" || prop == "textContent") {
                return Type::string();
            }
            return Type::dynamic();
        case HostObjectKind::BroDisplay:
            if (prop == "id" || prop == "width" || prop == "height" || prop == "scale" ||
                prop == "refreshRate" || prop == "x" || prop == "y") {
                return Type::number();
            }
            if (prop == "name") return Type::string();
            return Type::dynamic();
        case HostObjectKind::BroSysInfo:
            if (prop == "uptime" || prop == "cores" || prop == "totalMemory" ||
                prop == "freeMemory" || prop == "loadAvg") {
                return Type::number();
            }
            if (prop == "hostname" || prop == "os" || prop == "arch" || prop == "kernel") {
                return Type::string();
            }
            return Type::dynamic();
    }
    return Type::dynamic();
}

Type classifyHostMemberReadType(const ast::MemberAccess& m,
                                const UserBindingPredicate& isUserBound,
                                const HostObjectLookup& lookupHostObj,
                                bool mathPristine) {
    if (m.optional || m.isPrivate || m.property.empty()) return Type::dynamic();

    if (const auto* baseId = ast::exactAs<const ast::Ident*>(m.object.get())) {
        if (lookupHostObj) {
            const HostObjectKind kind = lookupHostObj(baseId->name);
            if (kind != HostObjectKind::None) {
                const Type propT = hostObjectPropertyType(kind, m.property);
                if (!propT.is(TypeKind::Dynamic)) return propT;
            }
        }
        if (!isUserBound || !isUserBound(baseId->name)) {
            if (baseId->name == "Math" && mathPristine && isMathValueProperty(m.property)) {
                return Type::number();
            }
            if (isHostGlobalNumericProperty(baseId->name, m.property)) {
                return Type::number();
            }
        }
        return Type::dynamic();
    }

    if (const auto* baseCall = ast::exactAs<const ast::Call*>(m.object.get())) {
        const HostObjectKind kind = classifyHostCallReturnedObject(*baseCall, isUserBound);
        if (kind != HostObjectKind::None) {
            return hostObjectPropertyType(kind, m.property);
        }
        return Type::dynamic();
    }

    if (const auto* outerMem = ast::exactAs<const ast::MemberAccess*>(m.object.get())) {
        if (outerMem->optional || outerMem->isPrivate) return Type::dynamic();
        const auto* rootId = ast::exactAs<const ast::Ident*>(outerMem->object.get());
        if (rootId == nullptr) return Type::dynamic();
        if (isUserBound && isUserBound(rootId->name)) return Type::dynamic();
        if (rootId->name == "bro" &&
            isBroNamespaceNumericProperty(outerMem->property, m.property)) {
            return Type::number();
        }
        if ((rootId->name == "window" || rootId->name == "globalThis" || rootId->name == "self") &&
            isHostGlobalNumericProperty(outerMem->property, m.property)) {
            return Type::number();
        }
    }

    return Type::dynamic();
}

HostNullability classifyHostCallNullability(const ast::Call& call,
                                            const UserBindingPredicate& isUserBound) {
    if (call.optional) return HostNullability::MaybeNullish;
    const auto* member = ast::exactAs<const ast::MemberAccess*>(call.callee.get());
    if (member == nullptr || member->optional || member->isPrivate) {
        return HostNullability::Unknown;
    }

    // Known DOM element/selector/attribute lookup methods that return `Element | null`
    // or `string | null` on any receiver or `document`.
    if (member->property == "getElementById" || member->property == "querySelector" ||
        member->property == "closest" || member->property == "getAttribute" ||
        member->property == "getAttributeNS" || member->property == "getAttributeNode" ||
        member->property == "getSelection") {
        if (const auto* baseId = ast::exactAs<const ast::Ident*>(member->object.get())) {
            if (isUserBound && isUserBound(baseId->name) &&
                (baseId->name == "document" || baseId->name == "window")) {
                return HostNullability::Unknown;
            }
        }
        return HostNullability::MaybeNullish;
    }

    if (member->property == "getBoundingClientRect" || member->property == "getClientRects" ||
        member->property == "querySelectorAll" || member->property == "getElementsByTagName" ||
        member->property == "getElementsByClassName") {
        return HostNullability::DefinitelyNonNullish;
    }

    if (const auto* baseId = ast::exactAs<const ast::Ident*>(member->object.get())) {
        if (isUserBound && isUserBound(baseId->name)) return HostNullability::Unknown;
        if (baseId->name == "document") {
            if (member->property == "createElement" || member->property == "createElementNS" ||
                member->property == "createTextNode" ||
                member->property == "createDocumentFragment" ||
                member->property == "createComment") {
                return HostNullability::DefinitelyNonNullish;
            }
            if (member->property == "elementFromPoint") {
                return HostNullability::MaybeNullish;
            }
        }
        if (baseId->name == "localStorage" || baseId->name == "sessionStorage") {
            if (member->property == "getItem" || member->property == "key") {
                return HostNullability::MaybeNullish;
            }
        }
        if (baseId->name == "window" || baseId->name == "globalThis") {
            if (member->property == "getComputedStyle") {
                return HostNullability::DefinitelyNonNullish;
            }
            if (member->property == "open") {
                return HostNullability::MaybeNullish;
            }
        }
        if (baseId->name == "Object") {
            if (member->property == "keys" || member->property == "values" ||
                member->property == "entries" || member->property == "assign" ||
                member->property == "create" || member->property == "fromEntries") {
                return HostNullability::DefinitelyNonNullish;
            }
        }
        if (baseId->name == "Array") {
            if (member->property == "from" || member->property == "of") {
                return HostNullability::DefinitelyNonNullish;
            }
        }
        return HostNullability::Unknown;
    }

    if (const auto* outerMem = ast::exactAs<const ast::MemberAccess*>(member->object.get())) {
        if (outerMem->optional || outerMem->isPrivate) return HostNullability::Unknown;
        const auto* rootId = ast::exactAs<const ast::Ident*>(outerMem->object.get());
        if (rootId == nullptr || (isUserBound && isUserBound(rootId->name))) {
            return HostNullability::Unknown;
        }
        if (rootId->name == "bro") {
            if (outerMem->property == "displays" &&
                (member->property == "primary" || member->property == "byId")) {
                return HostNullability::MaybeNullish;
            }
            if (outerMem->property == "compositor" &&
                (member->property == "focusedSurface" || member->property == "surfaceById")) {
                return HostNullability::MaybeNullish;
            }
            if ((outerMem->property == "displays" && member->property == "list") ||
                (outerMem->property == "sys" && member->property == "info") ||
                (outerMem->property == "theme" &&
                 (member->property == "current" || member->property == "palette"))) {
                return HostNullability::DefinitelyNonNullish;
            }
        }
    }

    return HostNullability::Unknown;
}

HostNullability classifyHostPropertyNullability(const ast::MemberAccess& member,
                                                const UserBindingPredicate& isUserBound) {
    if (member.optional) return HostNullability::MaybeNullish;
    if (member.isPrivate) return HostNullability::Unknown;

    if (const auto* baseId = ast::exactAs<const ast::Ident*>(member.object.get())) {
        if (baseId->name == "document" && (!isUserBound || !isUserBound("document"))) {
            if (member.property == "activeElement" || member.property == "fullscreenElement" ||
                member.property == "pointerLockElement" || member.property == "scrollingElement") {
                return HostNullability::MaybeNullish;
            }
            if (member.property == "body" || member.property == "head" ||
                member.property == "documentElement") {
                return HostNullability::DefinitelyNonNullish;
            }
        }
    }

    // Standard DOM node navigation properties that can always evaluate to `null`
    // at tree boundaries.
    if (member.property == "parentElement" || member.property == "parentNode" ||
        member.property == "firstElementChild" || member.property == "lastElementChild" ||
        member.property == "nextElementSibling" || member.property == "previousElementSibling" ||
        member.property == "firstChild" || member.property == "lastChild" ||
        member.property == "nextSibling" || member.property == "previousSibling" ||
        member.property == "offsetParent") {
        return HostNullability::MaybeNullish;
    }

    return HostNullability::Unknown;
}

std::string describeHostNullabilitySource(const ast::Expr& expr) {
    if (const auto* call = ast::exactAs<const ast::Call*>(&expr)) {
        if (const auto* m = ast::exactAs<const ast::MemberAccess*>(call->callee.get())) {
            if (const auto* baseId = ast::exactAs<const ast::Ident*>(m->object.get())) {
                return baseId->name + "." + m->property + "(...)";
            }
            if (const auto* outerMem = ast::exactAs<const ast::MemberAccess*>(m->object.get())) {
                if (const auto* rootId = ast::exactAs<const ast::Ident*>(outerMem->object.get())) {
                    return rootId->name + "." + outerMem->property + "." + m->property + "(...)";
                }
            }
            return m->property + "(...)";
        }
    }
    if (const auto* m = ast::exactAs<const ast::MemberAccess*>(&expr)) {
        if (const auto* baseId = ast::exactAs<const ast::Ident*>(m->object.get())) {
            return baseId->name + "." + m->property;
        }
        return "." + m->property;
    }
    return "nullable expression";
}

}  // namespace bronze::types
