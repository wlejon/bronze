// Two bindings and a function, so the namespace below has an export that is a
// value, one that is a constant, and one that is callable — and a name that is
// not exported at all is `missing`, which nothing here declares.
export let live = 1;
export const fixed = 2;

export function bump() {
  live += 1;
}
