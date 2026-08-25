#pragma once

#include <cstdint>
#include <map>
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
//
// `<kind>` is `number` or `numeric-elements`. `<class>` is matched on its LAST
// dotted component, because the module linker renames every module-level
// binding into one namespace before inference runs (`Matrix4` is `mod1.Matrix4`
// by then) and a manifest is written against the source, not against a link
// ordering. Two modules declaring the same class name therefore share one
// entry; that is the cost of not making the manifest depend on link order.
//
// A `*` field entry covers every field of the class, and a pin on a base class
// covers its subclasses — a subclass instance HAS the base's fields. An exact
// field entry wins over the class's `*`.
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

    bool empty() const { return byClass_.empty(); }
    // Entries, counting a `*` as one. Diagnostics only.
    size_t size() const;

private:
    // class base name -> field name (or "*") -> kind.
    std::map<std::string, std::map<std::string, PinKind>> byClass_;
};

}  // namespace bronze::types
