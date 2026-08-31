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
//
// The second table the same proof pays for is METHOD BODIES. `X.m(...)` on a
// certified literal calls a function this pass can read with a receiver it can
// name, so a call site may run part of that body in place of making the call —
// and what a guarded early return costs, on a body that always takes it, is
// the two call frames and nothing else.
//
// A method is described here as the shape a call site can evaluate: a run of
// `if (COND) return EXPR;` clauses, and then either a final `return EXPR;`
// (the whole body) or nothing (the rest of the body is not expressible, and
// the site calls the method as it always did once every guard has answered
// no). Only the expressions below appear in either half, and the restriction
// is what makes the substitution a substitution rather than a translation:
//
//   * a PARAMETER of the method, which the site has already evaluated into a
//     value — so argument order and argument effects are the call's own;
//   * `this.<key>` for a key this literal defines as a DATA property, which is
//     one own-slot read of the object and therefore free of user code: no
//     getter to run, no prototype to walk, nothing to observe a second read.
//     That last part is what a guard needs, because a site that falls through
//     to the real call evaluates the guard twice;
//   * `this.<key>(args)`, which is a call on the same object the site already
//     holds — either this same body again, inlined, or the method call the
//     original body would have made;
//   * literals, `!`, `typeof`, `===`, `!==`, `&&`, `||` and `?:` over those.
//
// A FREE IDENTIFIER is not in the list, deliberately: it names a binding in the
// literal's scope, and a call site is not in that scope. `==` is not in the
// list either — abstract equality can reach a `valueOf`, and a guard must be
// repeatable.
//
// `this` as a VALUE is not in the list because the certification above already
// refuses the whole literal for it: a method that hands `this` on hands the
// object to code the escape rule cannot follow.
struct ModuleLiteralInline {
    // `if (condition) return result;` — `result` null is a bare `return;`.
    struct Guard {
        const ast::Expr* condition = nullptr;
        const ast::Expr* result = nullptr;
    };

    const ast::FunctionExpr* fn = nullptr;
    // Positional, and the site binds argument i to parameter i. Every one is a
    // plain name: a default, a rest and a pattern each make the binding step
    // code of its own, which is a frame the site cannot skip.
    std::vector<std::string> params;
    std::vector<Guard> guards;
    // The body's last expression, when the body is entirely expressible here.
    // Null means the site takes `tail` below.
    const ast::Expr* tail = nullptr;
    // What a site does when no guard answered: call the method exactly as it
    // does today (true), or produce `undefined`, which is what falling off the
    // end of a body produces (false).
    bool tailIsCall = false;
};

class ModuleLiteralFacts {
public:
    void scan(const ast::Module& module);

    // The data property `<binding>.<name>` may be read in place of, or null.
    const std::string* backingKey(const std::string& binding, const std::string& name) const;

    // The body `<binding>.<name>(...)` may run in place of calling, or null.
    const ModuleLiteralInline* inlinableMethod(const std::string& binding,
                                               const std::string& name) const;

    // One line per forwarded property, `<binding>.<accessor> -> <backing>`, in
    // one order on every machine. This is the only way to see from outside
    // whether the proof held on a given program, so `--infer-stats` prints it.
    std::vector<std::string> report() const;

    // One line per inlinable method, naming how much of the body a site runs.
    std::vector<std::string> inlineReport() const;

private:
    // binding -> accessor name -> backing data-property name. Ordered, so that
    // anything that walks it walks it in one order on every machine.
    std::map<std::string, std::map<std::string, std::string>> forward_;
    // binding -> method name -> the shape a site may evaluate. Ordered for the
    // same reason.
    std::map<std::string, std::map<std::string, ModuleLiteralInline>> inline_;
};

// THE SEAM: `BRONZE_NO_MODULE_LITERAL_DEVIRT=1` leaves both tables empty, and
// every accessor read and every method call is the call it was.
bool moduleLiteralDevirtDisabled();

// The narrower seam: `BRONZE_NO_MODULE_LITERAL_INLINE=1` leaves only the
// method table empty, so accessor forwarding stays and no body moves.
bool moduleLiteralInlineDisabled();

}  // namespace bronze::types
