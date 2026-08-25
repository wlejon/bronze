#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace bronze::types {

// What one manifest entry DECLARES about a (class, field) pair.
enum class PinKind : uint8_t {
    // The slot holds a Number at every read. Spends the field-type claim
    // without the builtHere / fieldValueCandidate / write-audit proofs, which
    // is what makes the read a raw unbox instead of a checked one.
    Number,
    // The slot holds a dense JS Array of Numbers, indexed in bounds. The
    // element form this licenses (`il::kElemKindPlainArrayF64`) emits no guard
    // of any kind — no tag test, no bounds check, no hole check.
    NumericElements,
    // The slot holds a Number, `null` or `undefined`, and nothing else — an
    // uninitialized field, or an optional argument the constructor stored.
    //
    // A WEAKER promise than `Number`, and it buys a weaker thing. The lattice
    // has no unions (types/type.h says why), so this never becomes a type: the
    // read stays `Dynamic` and every boxed consumer of it — `typeof`, `===
    // null`, a call argument — is the dynamic one it always was. What the pin
    // licenses is the COERCING position, where the value is about to become a
    // double anyway: bronze's NaN box puts every Number at or below
    // `kNumberMaxBits`, so "is this the number arm?" is one unsigned compare
    // and the nullish arm is two constants (ToNumber(null) = +0,
    // ToNumber(undefined) = NaN). Branchless, in registers, no helper call.
    //
    // Unlike `Number` this pin cannot be spent on a raw unbox anywhere else,
    // which is the point: a raw unbox of the `undefined` singleton reads
    // 0xFFF6000000000000 as a double, and the whole reason the flat lattice
    // refuses `number | undefined` is that nothing downstream would notice.
    NumberOrNullish,
};

// The `--pins` manifest: per-(class, field) declarations inference is told to
// believe.
//
// A pin is NOT a proof and nothing here derives one. It is a promise the
// INVOCATION makes about the program, the same family of promise as
// `--host-globals` and `--assume-no-bigint`: unchecked at the read, and
// destined to be enforced on the WRITE paths once that machinery exists. Until
// then the manifest is the whole of the enforcement, which is why it is an
// explicit per-(class, field) list and never a heuristic — the blanket form
// (`BRONZE_UNSOUND_PINS`, kept for measurement continuity) pins fields whose
// writes are not numeric at all, and a program that reads one of those reads
// a pointer's bits as a double.
//
// Grammar — one entry per line, `#` starts a comment, blank lines ignored:
//
//     <class>.<field>: <kind>
//     <class>.*: <kind>
//     function <function>.<binding>: number
//     param <owner>(<parameter>): number
//     return <owner>: number
//
// `<kind>` is `number`, `numeric-elements` or `number-or-nullish`. `<class>` is
// matched on its LAST
// dotted component, because the module linker renames every module-level
// binding into one namespace before inference runs (`Matrix4` is `mod1.Matrix4`
// by then) and a manifest is written against the source, not against a link
// ordering. Two modules declaring the same class name therefore share one
// entry; that is the cost of not making the manifest depend on link order.
//
// A `*` field entry covers every field of the class, and a pin on a base class
// covers its subclasses — a subclass instance HAS the base's fields. An exact
// field entry wins over the class's `*`.
//
// The `function` form pins a CAPTURED BINDING rather than a field: the slot of
// a `let`/`const` declared at the top level of the named function's body and
// read or written by closures inside it. `WebGLState`'s hot state is seven such
// bindings and no fields at all, so the field form reaches none of it.
//
// It exists for the bindings the SOUND proof (lower_scope.cpp
// `planEnvSlotNumberTypes`) refuses only because a written value is unproven —
// a parameter, typically. There is no `*` form and no kind but `number`: an env
// slot pin is spent on a raw unbox at every read of that slot, so the promise
// has to be exact, and one wrong name is a pointer's bits read as a double.
// `<function>` is matched on its last dotted component, for the reason
// `<class>` is.
//
// The `param` and `return` forms pin one position of a CALLING CONVENTION: the
// named parameter becomes an f64 slot of the function's typed entry
// (llvm_backend.cpp `declareEntries`), and the return an f64 result. `<owner>`
// is the function as the source spells it — `setBlending` for a plain function
// or closure, `Matrix4.multiplyMatrices` for a class method — and it matches an
// IL function whose name IS it or ENDS WITH `.` + it, which is the same
// last-components rule the forms above use and for the same reason.
//
// This is the WEAKEST pin in the file, and the only one whose miss is not a
// wrong read of bits. The uniform convention still enters through the call
// wrapper, and the wrapper converts with ToNumber (7.1.4) — inline for the
// number case, `bronze_unbox_f64` for everything else — so a non-number that
// reaches a pinned parameter is COERCED, never reinterpreted. What the promise
// buys is the typed slot; what a wrong entry costs is JS semantics (`f("5")`
// sees 5, and `x === "5"` inside the body is then false), not a pointer read as
// a double.
//
// Only `number`, and only a parameter the caller always supplies a value for: a
// parameter with a default, a destructuring pattern, or `...rest` is refused,
// because there is no `undefined` in an f64 and each of those three is a
// position whose value the language may synthesize. A pinned RETURN on a body
// that can fall off its end is refused by lowering for the same reason.
class PinManifest {
public:
    // Parses manifest text. `path` appears only in the error message. Returns
    // false on a malformed line with `err` naming the line, never a silent
    // skip: a typo in a manifest that licenses unguarded loads must not read
    // as "that field is not pinned".
    bool parse(const std::string& text, const std::string& path, std::string& err);

    // The pin declared for `className.field`, or null. `className` may carry
    // the module linker's prefix.
    const PinKind* lookup(const std::string& className, const std::string& field) const;

    // Is `functionName.binding` pinned as a numeric env slot? `functionName`
    // may carry the module linker's prefix.
    bool envSlotPinned(const std::string& functionName, const std::string& binding) const;

    // Is the parameter `param` of the IL function `ilName` pinned Number?
    // `ilName` is the IL function's whole name; the entry may spell any suffix
    // of it that begins at a dot boundary.
    bool paramPinned(const std::string& ilName, const std::string& param) const;
    // Is the IL function `ilName` pinned to return a Number? Same matching.
    bool returnPinned(const std::string& ilName) const;

    bool empty() const {
        return byClass_.empty() && byFunction_.empty() && paramsBySignature_.empty() &&
               pinnedReturns_.empty();
    }
    // Entries, counting a `*` as one. Diagnostics only.
    size_t size() const;

private:
    // class base name -> field name (or "*") -> kind.
    std::map<std::string, std::map<std::string, PinKind>> byClass_;
    // function base name -> the captured bindings of its body pinned Number.
    std::map<std::string, std::set<std::string>> byFunction_;
    // `<owner>` as the manifest spelled it -> the parameter names pinned
    // Number. Keyed on the whole spelling, dots and all, because a `param`
    // entry's owner may be one component (`setBlending`) or two
    // (`Matrix4.multiplyMatrices`) and the lookup tries the IL name's suffixes
    // against it rather than the other way round.
    std::map<std::string, std::set<std::string>> paramsBySignature_;
    std::set<std::string> pinnedReturns_;
};

}  // namespace bronze::types
