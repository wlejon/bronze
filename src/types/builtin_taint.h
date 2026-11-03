#pragma once

#include <string>

#include "ast/ast.h"

namespace bronze::types {

// Can any statement in the program change what a builtin global's name
// answers, or what it answers for the members a claim site folds? Run over the
// MERGED module — the whole program — so one pass answers for every site.
//
// Two flavours, by what a claim about the builtin rests on:
//
//   - `Math` (aliasTaints = true): its METHODS are claimed, and a method is a
//     writable property, so the object itself must not escape. A bare mention
//     taints (it aliases the object, and a later write through the alias is
//     invisible here), as does a member write, update or delete through it.
//   - `Number` (aliasTaints = false): only its 21.1.2 value properties are
//     claimed (`Number.EPSILON`), and every one of them is non-writable and
//     non-configurable, so nothing done to the OBJECT can change them — `Number`
//     is passed around and called (`Number(x)`) freely. What can change the
//     answer is the NAME: a sloppy `Number = f`, or the global object's
//     `Number` property rebound.
//
// Both taint on what rebinds the name: an assignment to the bare name, a
// destructuring that could target it, any mention of `globalThis` (the other
// road to the global object) other than a named member that leads nowhere near
// it (`globalThis.BENCH_CONFIG`), a member of `window`, `self`, `top`, `parent`
// or `frames` (the same object in bro) that names the builtin or is computed,
// and the evaluators. A program binding NAMED like the builtin needs no care
// here: each claim site separately refuses shadowed names.
//
// Sloppy-mode `this` is not a vector: bronze binds it undefined, never the
// global object (lowerThisValue), so `this.Math` reaches nothing.
bool builtinTainted(const ast::Module& module, const std::string& name, bool aliasTaints);

}  // namespace bronze::types
