// A module's top level runs inside the program's merged top level, so its
// frames must still name this file — and the importer's must name the importer.
export const topLevelStack = new Error("helper top level").stack;

export function helperThrows() {
  const before = 1;
  return before + missingInHelper;
}

// The linker renames this file's module-scope bindings; none of the names a
// program can observe may show the rename.
export class Shape {
  area() {
    return new Error("in method").stack;
  }
}
export const arrow = () => 1;
export let expr = function () {};
let assigned;
assigned = function () {};
export { assigned };
export function* gen() {}
export async function later() {}
function local() {}
export const localName = local.name;
export default function () {}
