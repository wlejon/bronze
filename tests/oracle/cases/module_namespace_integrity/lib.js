// One mutable binding, one constant, and a function that writes the mutable
// one — so the namespace has something whose `writable: true` is not a
// formality, and a live read to prove it after the copies below are taken.
//
// The names sort ('b' < 'c' < 'k') into an order that is not the declaration
// order, so nothing here passes by accident under no sort at all.
export let k = 1;
export const c = 2;

export function bump() {
  k += 1;
}
