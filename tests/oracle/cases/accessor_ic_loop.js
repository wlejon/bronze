// Test property inline caching for getters and setters (both own and prototype chain).

class Point {
  constructor(x, y) {
    this._x = x;
    this._y = y;
  }
  get x() {
    return this._x;
  }
  set x(val) {
    this._x = val;
  }
  get y() {
    return this._y;
  }
  set y(val) {
    this._y = val;
  }
  get magSq() {
    return this._x * this._x + this._y * this._y;
  }
}

class Point3D extends Point {
  constructor(x, y, z) {
    super(x, y);
    this._z = z;
  }
  get z() {
    return this._z;
  }
  set z(val) {
    this._z = val;
  }
  get magSq3D() {
    return this.magSq + this._z * this._z;
  }
}

let sum = 0;
const p = new Point(3, 4);
for (let i = 0; i < 1000; i++) {
  p.x = p.x + 1;
  p.y = p.y + 1;
  sum += p.magSq;
}
console.log(p.x);
console.log(p.y);
console.log(sum);

const p3 = new Point3D(1, 2, 3);
let sum3D = 0;
for (let i = 0; i < 1000; i++) {
  p3.x = p3.x + 1;
  p3.z = p3.z + 1;
  sum3D += p3.magSq3D;
}
console.log(p3.x);
console.log(p3.z);
console.log(sum3D);
