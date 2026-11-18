// The receiver of a SLOPPY function (10.2.1.2 OrdinaryCallBindThis): a call
// with no receiver, or with null or undefined, sees the global object, and a
// primitive receiver is boxed. d3 v3 and OpenLayers start with
// `!function () { this.d3 = ... }()`. A strict function and a module keep the
// receiver as passed.

!function () { globalThis.OUT = (this === globalThis) + ' ' + typeof this; }();
console.log(OUT);

function sloppy() { return this; }
function strict() { 'use strict'; return this; }
console.log(sloppy() === globalThis, strict());
console.log(sloppy.call(null) === globalThis, sloppy.call(undefined) === globalThis);
console.log(strict.call(null), strict.call(undefined));

console.log(typeof sloppy.call(5), typeof strict.call(5));
console.log(sloppy.call('s') instanceof String, sloppy.call(true) instanceof Boolean);
console.log((function () { return typeof this; }).call(5));
console.log((function () { return this; }).apply(null) === globalThis);

const bound = sloppy.bind(null);
console.log(bound() === globalThis);

const o = { m: sloppy };
const m = o.m;
console.log(m() === globalThis, o.m() === o);

[1].forEach(function () { console.log('callback', this === globalThis); });
[1].forEach(function () { console.log('callback with 7', typeof this, this == 7); }, 7);

// An arrow takes the receiver of the function it is written in.
function outer() { return (() => this)(); }
console.log(outer() === globalThis, outer.call(4) instanceof Number);

// A method of a class is strict code.
class K { m() { return this; } }
const km = new K().m;
console.log(km());

// Writing through the bound receiver writes the global.
function setG() { this.fromSloppy = 42; }
setG();
console.log(globalThis.fromSloppy);
