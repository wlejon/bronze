import { a } from './a.js';

export function b(n) {
  return n <= 0 ? 0 : a(n - 1) + 1;
}
