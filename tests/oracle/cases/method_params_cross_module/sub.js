import { Base } from './base.js';

// Inherits `m` and `twice`; a call on a Sub is a call of Base's methods.
export class Sub extends Base {}

// Overrides `describe`: a call on a receiver typed Base can reach this one.
export class Loud extends Base {
  describe(v) { return 'loud:' + String(v).toUpperCase(); }
}
