// AN INSTANCE FIELD INSTALLED BY A DESCRIPTOR, READ THROUGH THE CLASS.
//
// `Object.defineProperties(this, {position: {value: v}})` is how three.js's
// `Object3D` installs `position`, `rotation`, `quaternion` and `scale`, from
// `const` bindings the constructor makes just above the call — so it is the
// shape every scene-graph read in the milestone goes through. The class LAYOUT
// models the install (src/types/class_layout.cpp) and the program-wide write
// audit sees the descriptor's value as a write (src/types/field_audit.cpp), so
// the slot has a position and a claim about it can be made; what the answers
// below have to be right about is what is IN it.
//
// A descriptor states its own attributes, and an unstated one is not the one an
// assignment would have given: `writable` defaults to FALSE (6.2.6.5), so a
// strict store into `position` is a TypeError while a sloppy one is silently
// dropped. `configurable: true` then lets the slot be redefined out from under
// every read of it — to another instance of the same class, to a plain object
// with the same names, to those names in another order, to a number, to null,
// and to an accessor. A descriptor with no `value` at all installs the slot
// holding `undefined`, and one that is an accessor installs no slot value to
// speak of. Each line pins the answer for what is actually there.
//
// This file is a Script; the class bodies in it are strict code (15.7).

class Vec {
  constructor(x, y, z) {
    this.x = x;
    this.y = y;
    this.z = z;
  }
}

class Node3D {
  constructor() {
    // The three.js shape: a `const` above the call, installed by name.
    const position = new Vec(1, 2, 3);
    Object.defineProperties(this, {
      position: { configurable: true, enumerable: true, value: position },
      scale: { configurable: true, enumerable: true, value: new Vec(4, 5, 6) },
      tag: { configurable: true, enumerable: true, value: 'n' }
    });
    this.id = 7;
  }
  sum() {
    return this.position.x + this.position.y + this.position.z;
  }
  scaled() {
    return this.scale.x * 10 + this.scale.y * 10 + this.scale.z * 10;
  }
}

// A subclass adds its own fields AFTER the base's, so the base's slots keep
// their positions and the same reads serve both.
class Mesh extends Node3D {
  constructor() {
    super();
    this.visible = true;
  }
}

console.log('sum|' + new Node3D().sum());
console.log('scaled|' + new Node3D().scaled());
console.log('sub|' + new Mesh().sum() + '|' + new Mesh().scaled());
console.log('tag|' + new Node3D().tag + '|' + new Node3D().id);

// 11.2.1.2 / 10.1.6.1: the properties come out in creation order, and
// `defineProperties` creates them in the order the descriptor literal lists.
console.log('keys|' + Object.keys(new Node3D()).join(','));

// The descriptor states no `writable`, so 6.2.6.5 leaves it FALSE. A strict
// assignment to it is a TypeError (13.15.2, 6.2.5.6); the slot is unchanged
// either way.
function looseAssign() {
  const n = new Node3D();
  n.position = 5;
  return String(n.sum());
}

console.log('loose|' + looseAssign());

function strictAssign() {
  'use strict';
  const n = new Node3D();
  let name = 'no-throw';
  try {
    n.position = 5;
  } catch (e) {
    name = e.name;
  }
  return name + '|' + String(n.sum());
}

console.log('strict|' + strictAssign());

// `configurable: true`, so the slot CAN be redefined — to another instance of
// the same class, to a plain object with the same names, to a number, and to an
// accessor. Each read has to answer for what is there.
function redefinedTo(value) {
  const n = new Node3D();
  Object.defineProperty(n, 'position', { configurable: true, value: value });
  return String(n.sum());
}

console.log('same-class|' + redefinedTo(new Vec(10, 20, 30)));
console.log('literal|' + redefinedTo({ x: 9, y: 8, z: 7 }));
console.log('reordered|' + redefinedTo({ z: 90, y: 80, x: 70 }));
console.log('number|' + redefinedTo(5));
console.log('nullish|' + (function () {
  const n = new Node3D();
  Object.defineProperty(n, 'position', { configurable: true, value: null });
  try {
    return String(n.sum());
  } catch (e) {
    return e.name;
  }
})());

function redefinedToAccessor() {
  const n = new Node3D();
  const calls = [];
  Object.defineProperty(n, 'position', {
    configurable: true,
    get: function () {
      calls.push('g');
      return new Vec(100, 200, 300);
    }
  });
  return String(n.sum()) + '|' + calls.length;
}

console.log('accessor|' + redefinedToAccessor());

// A descriptor with no `value` field at all creates the property holding
// `undefined` (6.2.6.5 gives an absent [[Value]] the default `undefined`), so
// the class has a slot there and nothing to say about what it holds.
class Blank {
  constructor() {
    Object.defineProperty(this, 'slot', { configurable: true, enumerable: true });
    this.after = 3;
  }
}

function blank() {
  const b = new Blank();
  let name = 'no-throw';
  try {
    void b.slot.x;
  } catch (e) {
    name = e.name;
  }
  return String(b.slot) + '|' + name + '|' + b.after + '|' + Object.keys(b).join(',');
}

console.log('blank|' + blank());

// An ACCESSOR descriptor at install time: there is no slot value to harvest,
// and the read is a call.
class Lazy {
  constructor() {
    let made = 0;
    Object.defineProperty(this, 'built', {
      configurable: true,
      enumerable: true,
      get: function () {
        made = made + 1;
        return made;
      }
    });
  }
}

function lazy() {
  const l = new Lazy();
  return String(l.built) + '.' + String(l.built) + '.' + String(l.built);
}

console.log('lazy|' + lazy());

// `Object.defineProperty` singular, with the value in a `const` above it, and
// with a computed key the harvest cannot read — the second installs a slot the
// class knows nothing about, which must still read correctly.
class Singular {
  constructor(key) {
    const held = new Vec(2, 4, 8);
    Object.defineProperty(this, 'held', { configurable: true, enumerable: true, value: held });
    Object.defineProperty(this, key, { configurable: true, enumerable: true, value: 11 });
  }
  total() {
    return this.held.x + this.held.y + this.held.z;
  }
}

const sg = new Singular('extra');
console.log('singular|' + sg.total() + '|' + sg.extra + '|' + Object.keys(sg).join(','));
