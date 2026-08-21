// The layout claim, and the four things it has to reproduce: slot order across
// an `extends` chain, key ENUMERATION order (ECMA-262 6.1.7.1 insertion order),
// a warm read/write loop on a proven receiver, and one site reached by two
// different classes.

class Vec3 {
  constructor(x, y, z) {
    this.x = x;
    this.y = y;
    this.z = z;
  }
  dot(o) {
    return this.x * o.x + this.y * o.y + this.z * o.z;
  }
  scale(k) {
    this.x *= k;
    this.y *= k;
    this.z *= k;
    return this;
  }
}

class Named extends Vec3 {
  constructor(n, x, y, z) {
    super(x, y, z);
    this.name = n;
  }
  tag() {
    return this.name + "/" + this.dot(this);
  }
}

const a = new Vec3(1, 2, 3);
const b = new Vec3(4, 5, 6);
console.log(a.dot(b));
console.log(Object.keys(a).join(","));

const n = new Named("p", 2, 3, 6);
console.log(n.tag());
console.log(Object.keys(n).join(","));

// One site, two receiver shapes. `dot` is declared on Vec3, and a Named has a
// fourth field, so the shape the site published first cannot describe both.
let mixed = 0;
for (let i = 0; i < 200; i++) {
  mixed += (i % 2 === 0 ? a : n).dot(b);
}
console.log(mixed);

// Warm enough that every cache and cell on the path is filled.
let s = 0;
for (let i = 0; i < 1000; i++) {
  const v = new Vec3(i, i + 1, i + 2);
  v.scale(2);
  s += v.dot(v);
}
console.log(s);

// A field whose value is another proven class: the read chains through two
// static slots.
class Body {
  constructor() {
    this.position = new Vec3(1, 1, 1);
    this.velocity = new Vec3(0, 0, 0);
    this.mass = 2;
  }
  step() {
    this.position.x += this.velocity.x;
    this.position.y += this.velocity.y;
    this.position.z += this.velocity.z;
  }
}
const body = new Body();
body.velocity.x = 3;
body.velocity.z = -1;
for (let i = 0; i < 100; i++) body.step();
console.log(body.position.x + "," + body.position.y + "," + body.position.z);
console.log(Object.keys(body).join(","));
