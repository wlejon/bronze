export const obj = {};
export const arr = [];
export const calls = [];
export function keyOf(k) {
  calls.push(k);
  return 'p_' + k;
}
