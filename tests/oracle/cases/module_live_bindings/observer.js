import { count } from './counter.js';

// A second importer of the same binding. Two modules holding COPIES would
// agree with each other and disagree with the exporter; two modules holding
// views of one binding agree with everything.
export function observed() {
  return count;
}
