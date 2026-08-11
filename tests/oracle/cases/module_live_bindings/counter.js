export let count = 0;

export function bump() {
  count += 1;
  return count;
}

// A `const` initialised from `count` at module evaluation time. It is a
// SNAPSHOT: nothing links it to `count` afterwards, which is what makes the
// last line of main.js the control for the three above it.
export const frozen = count;
