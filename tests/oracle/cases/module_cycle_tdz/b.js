import { marker } from './a.js';

// a.js has not been evaluated yet: this is the cycle's back edge read.
try {
  console.log('b sees: ' + marker);
} catch (e) {
  console.log('b sees: ' + e.name);
}

export function readLate() {
  return marker;
}

export const fromB = 'b-ready';
