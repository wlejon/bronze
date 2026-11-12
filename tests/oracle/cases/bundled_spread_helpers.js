// esbuild's lowering of object spread and object rest, as a bundle carries it:
// the linker turns calls of helpers that are exactly esbuild's back into the
// syntax (modules/bundled_spread.cpp). The results must be what the helpers
// themselves compute, and a helper that is not esbuild's must be left to run.
var __defProp$1 = Object.defineProperty;
var __getOwnPropSymbols$2 = Object.getOwnPropertySymbols;
var __hasOwnProp$2 = Object.prototype.hasOwnProperty;
var __propIsEnum$2 = Object.prototype.propertyIsEnumerable;
var __defNormalProp$1 = (obj, key, value) => key in obj ? __defProp$1(obj, key, { enumerable: true, configurable: true, writable: true, value }) : obj[key] = value;
var __spreadValues$1 = (a, b) => {
  for (var prop in b || (b = {}))
    if (__hasOwnProp$2.call(b, prop))
      __defNormalProp$1(a, prop, b[prop]);
  if (__getOwnPropSymbols$2)
    for (var prop of __getOwnPropSymbols$2(b)) {
      if (__propIsEnum$2.call(b, prop))
        __defNormalProp$1(a, prop, b[prop]);
    }
  return a;
};
var __objRest$a = (source, exclude) => {
  var target = {};
  for (var prop in source)
    if (__hasOwnProp$2.call(source, prop) && exclude.indexOf(prop) < 0)
      target[prop] = source[prop];
  if (source != null && __getOwnPropSymbols$2)
    for (var prop of __getOwnPropSymbols$2(source)) {
      if (exclude.indexOf(prop) < 0 && __propIsEnum$2.call(source, prop))
        target[prop] = source[prop];
    }
  return target;
};

// The same text with one more statement: not esbuild's helper, so its calls
// stay calls and the count shows it ran.
var calls = 0;
var __hasOwnProp$3 = Object.prototype.hasOwnProperty;
var __getOwnPropSymbols$3 = Object.getOwnPropertySymbols;
var __propIsEnum$3 = Object.prototype.propertyIsEnumerable;
var __defProp$2 = Object.defineProperty;
var __defNormalProp$2 = (obj, key, value) => key in obj ? __defProp$2(obj, key, { enumerable: true, configurable: true, writable: true, value }) : obj[key] = value;
var __spreadValues$2 = (a, b) => {
  calls++;
  for (var prop in b || (b = {}))
    if (__hasOwnProp$3.call(b, prop))
      __defNormalProp$2(a, prop, b[prop]);
  if (__getOwnPropSymbols$3)
    for (var prop of __getOwnPropSymbols$3(b)) {
      if (__propIsEnum$3.call(b, prop))
        __defNormalProp$2(a, prop, b[prop]);
    }
  return a;
};

function show(label, o) {
    const parts = [];
    for (const k of Reflect.ownKeys(o)) {
        const d = Object.getOwnPropertyDescriptor(o, k);
        parts.push(String(k) + "=" + ("value" in d ? JSON.stringify(d.value) : "accessor") +
                   (d.enumerable && d.writable && d.configurable ? "" : "!"));
    }
    console.log(label + ": " + parts.join(" "));
}

const sym = Symbol("s");
const log = [];
const defaults = { color: 1, alpha: 0.5, texture: "white", 3: "three", [sym]: "sym" };
const source = {
    alpha: 2,
    get width() { log.push("get width"); return 7; },
    1: "one",
    toString: "own toString",
};
Object.defineProperty(source, "hidden", { value: 1, enumerable: false });

function toStyle(value, defaultStyle) {
    const _a = defaultStyle, { color, alpha } = _a, rest = __objRest$a(_a, ["color", "alpha"]);
    const fill = __spreadValues$1(__spreadValues$1({}, rest), value);
    return __spreadValues$1({ color, alpha, get late() { return 1; } }, fill);
}
show("style", toStyle(source, defaults));
show("style-null", toStyle(null, defaults));
show("style-string", toStyle("ab", defaults));
show("style-zero", toStyle(0, defaults));
console.log(log.join("|"));

// A target that is not a literal is the helper's own business: it is mutated
// in place and returned.
const existing = { kept: 1 };
const same = __spreadValues$1(existing, { added: 2 });
console.log(same === existing, JSON.stringify(existing));

// A shadowing binding is not the helper.
function shadowed() {
    const __spreadValues$1 = (a, b) => "shadow " + Object.keys(a) + Object.keys(b);
    return __spreadValues$1({ a: 1 }, { b: 2 });
}
console.log(shadowed());

// Rest over sources of other kinds, and with nothing excluded.
show("rest-all", __objRest$a(defaults, []));
show("rest-string", __objRest$a("xyz", ["1"]));
show("rest-array", __objRest$a(["p", "q"], ["0"]));
show("rest-null", __objRest$a(null, ["a"]));
log.length = 0;
show("rest-getter", __objRest$a(source, ["width", "toString"]));
console.log(log.length);

show("other", __spreadValues$2(__spreadValues$2({}, defaults), source));
console.log(calls);
