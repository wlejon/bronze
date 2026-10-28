// The other side of math_constants_fold: a Math that is not the builtin, or a
// module that defines a member on Math, gets no fold. A shadowing binding
// reads its own PI, and a module-wide definition taints every read.

function local() {
  const Math = { PI: 3, E: "e", SQRT2: undefined };
  return [Math.PI, Math.E, Math.SQRT2];
}
console.log(local());

function param(Math) { return Math.PI + Math.LN2; }
console.log(param({ PI: 1, LN2: 2 }), param({ PI: "a", LN2: "b" }));

// A new member on Math anywhere in the module forfeits the pristine proof;
// the reads still see the runtime's (unchanged) constants.
Math.TAU = Math.PI * 2;
console.log(Math.TAU, Math.PI, Math.E);

// Constants are non-configurable: redefinition throws.
try { Object.defineProperty(Math, "PI", { value: 3 }); console.log("redefined"); }
catch (e) { console.log(e.constructor.name, Math.PI); }

// A sloppy write to a constant is silently ignored; Number's are the same.
Math.E = 1;
Number.EPSILON = 1;
console.log(Math.E, Number.EPSILON);
const nd = Object.getOwnPropertyDescriptor(Number, "MAX_SAFE_INTEGER");
console.log(nd.value, nd.writable, nd.enumerable, nd.configurable);
try { delete Math.LN2; console.log("deleted?", Math.LN2); } catch (e) { console.log(e.constructor.name); }
