import * as ns from './m.js';
// A cycle back to the entry. This file runs first, so the entry's namespace
// is only read later, when the entry's own body calls `entryNs`.
import * as entry from './main.js';

export const nsA = ns;
export function entryNs() {
  return entry;
}
