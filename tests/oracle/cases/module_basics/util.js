export const label = 'util';

// Not exported, and named the same as a function in main.js. The two are
// different bindings in different module scopes, and each caller reaches its
// own.
function helper() {
  return 'util helper';
}

export function describe() {
  return helper();
}

export function twice(n) {
  return n * 2;
}

// An anonymous default export (ECMA-262 16.2.3.7): a hoisted declaration with
// no name, which only an importer can name.
export default function () {
  return 'the default';
}

const hidden = 'not exported';
// A renaming export of a name this module never publishes under its own
// spelling.
export { hidden as secret };
