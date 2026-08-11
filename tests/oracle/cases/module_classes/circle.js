import { Shape } from './shape.js';

// `extends` across a file boundary, and a `super.` call that has to reach the
// renamed parent's prototype (docs/0008, docs/0023 decision 1).
export class Circle extends Shape {
  constructor(r) {
    super('circle');
    this.r = r;
  }
  describe() {
    return super.describe() + ' r=' + this.r;
  }
  area() {
    return 3 * this.r * this.r;
  }
}

export const kind = 'circle';
