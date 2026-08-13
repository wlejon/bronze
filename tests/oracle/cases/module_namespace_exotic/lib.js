// Export names chosen so that DECLARATION order and CODE UNIT order disagree at
// every place a comparison could be got wrong: a capital against a lower-case
// letter ('Z' is 0x5A and 'a' is 0x61, so `Z` comes first), a prefix against
// what extends it (`parse` before `parseIt`), and the first-declared name last.
export let z = 1;
export const a = 2;
export const Z = 3;
export const parseIt = 4;
export const parse = 5;
export function bump() { z += 1; }
