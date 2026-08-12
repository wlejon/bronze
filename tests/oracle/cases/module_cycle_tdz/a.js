import { readLate, fromB } from './b.js';

export const marker = 'a-marker';

export function report() {
  return fromB + ' / ' + readLate();
}
