// BLOCKED: a class constructor's `prototype` is writable here. ECMA-262
// 15.7.14 step 16 (MakeConstructor with writablePrototype false) makes it
// non-writable and non-configurable, but FunctionHeader::prototype_readonly is
// set only for the built-in constructors (runtime/integrity.cpp
// rtFunctionPrototypeWritable), so a strict assignment replaces it instead of
// throwing. A redefinition through Object.defineProperty lands in the statics
// box (builtin_object_define.cpp applyToReceiver hands a Function its box),
// where no `prototype` read ever looks, and succeeds instead of throwing.
//
// Lifting it: set prototype_readonly when a class constructor is created, and
// answer [[DefineOwnProperty]] of `prototype` on a function from its header.
class P {}
function readPP() { return P.prototype; }
const p0 = readPP();
for (let i = 0; i < 50; i++) readPP();
const strictTry = (() => { 'use strict'; try { P.prototype = {}; return 'ok'; } catch (e) { return e.constructor.name; } })();
let redef = 'ok';
try { Object.defineProperty(P, 'prototype', { value: {} }); } catch (e) { redef = e.constructor.name; }
let same = 'ok';
try { Object.defineProperty(P, 'prototype', { value: p0 }); } catch (e) { same = e.constructor.name; }
console.log(readPP() === p0, strictTry, redef, same, Object.getOwnPropertyNames(P.prototype).join(','));
