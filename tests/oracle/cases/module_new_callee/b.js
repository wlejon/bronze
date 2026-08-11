// The same exported NAMES as a.js's under different values. Two module
// scopes, two bindings — which is what makes a missed rename observable
// rather than merely unresolved.
export function Ctor() {
  this.tag = 'b';
}
export const table = { Ctor: Ctor };
