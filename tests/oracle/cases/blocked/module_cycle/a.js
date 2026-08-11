import { b } from './b.js';

export function a(n) {
  return n <= 0 ? 0 : b(n - 1) + 1;
}
