export class Shape {
  constructor(name) {
    this.name = name;
  }
  describe() {
    return 'shape ' + this.name;
  }
}

// The same exported NAME as circle.js's, under a different value. Two module
// scopes, two bindings.
export const kind = 'base';
