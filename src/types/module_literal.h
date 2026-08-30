#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ast/ast.h"

namespace bronze::types {

// Which reads of an accessor on a module-scope object literal are the same
// expression as a read of the property the getter would have read.
//
// `const X = { get p() { return this._q; }, _q: <v>, ... };` at module scope
// defines `p` as an OWN accessor of the object the literal builds. `X.p`
// therefore finds the getter on `X` itself, calls it with `this === X`, and
// that body is one property read of `X`. So `X.p` and `X._q` denote the same
// value — not approximately. The getter's body IS the forwarded read, and
// everything the forwarded read can meet — `_q` deleted, `_q` an accessor of
// its own, `_q` answered off the prototype — it meets exactly as the getter's
// own read would have. What forwarding removes is the call, and nothing else.
//
// That equality holds only while `X.p` is still THAT getter and `X` is still
// THAT object, so the scan proves both, and it proves them by refusing every
// program it cannot follow:
//
//   * `X` is bound exactly once in the whole program. No other binding of the
//     name shadows it at a read site and nothing assigns it, so the identifier
//     at the read and the object the literal built are the same thing without
//     a scope walk.
//   * every mention of `X` is `X.name` or `X["name"]`. The object never
//     becomes a value other code holds, which is the only way
//     `Object.defineProperty`, `Reflect.defineProperty` or a `Proxy` could
//     reach it — each of them takes its target as an ARGUMENT — and the only
//     way an `X[k]` could name `p` at run time.
//   * every member the program CALLS on `X` is a function written inside the
//     literal, and is never also assigned. `X.m(...)` makes `this` be `X`
//     inside `m` without `X` ever having been a value, so it is the one way
//     past the rule above — and it is only safe while the compiler can read
//     the function that receives it.
//   * `this` never escapes those functions, and is never used to REPLACE one
//     of the members the program calls, because inside them `this` is `X` and
//     handing it on is handing on the object.
//   * `X.__defineGetter__` / `X.__defineSetter__`, which redefine through the
//     receiver rather than through an argument, appear nowhere.
//   * no `delete` anywhere in the program names `p`. A `delete o[k]` whose key
//     this pass cannot read needs no answer: its receiver cannot be `X`, since
//     `X` never escaped into one.
//
// The setter half is not this table's business. A WRITE through `X.p` must run
// the setter, and this answers reads.
class ModuleLiteralAccessors {
public:
    void scan(const ast::Module& module);

    // The data property `<binding>.<name>` may be read in place of, or null.
    const std::string* backingKey(const std::string& binding, const std::string& name) const;

    // One line per forwarded property, `<binding>.<accessor> -> <backing>`, in
    // one order on every machine. This is the only way to see from outside
    // whether the proof held on a given program, so `--infer-stats` prints it.
    std::vector<std::string> report() const;

private:
    // binding -> accessor name -> backing data-property name. Ordered, so that
    // anything that walks it walks it in one order on every machine.
    std::map<std::string, std::map<std::string, std::string>> forward_;
};

// THE SEAM: `BRONZE_NO_MODULE_LITERAL_DEVIRT=1` leaves the table empty, and
// every accessor read is the call it was.
bool moduleLiteralDevirtDisabled();

}  // namespace bronze::types
