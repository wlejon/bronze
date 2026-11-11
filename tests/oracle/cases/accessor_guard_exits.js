// Loops over objects whose every statement is an inlined getter or setter,
// with no arithmetic of their own. The guarded-region pass gives such a loop a
// fast copy in which a guard that misses leaves for the slow copy, where the
// access is the ordinary one. `step` is the loop over a list of receivers and
// `churn` the same loop on one receiver, `this`. Each run below makes some
// guard miss, most of them part way through a loop, and what the loop leaves
// behind has to be what the ordinary accesses leave.
let redefineAt = -1;
let growAt = -1;
let shadowAt = -1;
class Point {
  constructor(x, y) {
    this._x = x;
    this._y = y;
    this.writes = 0;
  }
  get x() {
    return this._x;
  }
  set x(v) {
    if (this._x !== v) {
      this._x = v;
      this.writes++;
    }
    if (v === growAt) this.grown = v;
  }
  get y() {
    return this._y;
  }
  set y(v) {
    this._y = v;
    if (v === shadowAt) {
      Object.defineProperty(this, 'x', { value: 5, writable: true, enumerable: true, configurable: true });
    }
    if (v === redefineAt) {
      Object.defineProperty(Point.prototype, 'x', {
        get() {
          return this._x + 0.25;
        },
        set(w) {
          this._x = w - 1;
        },
        configurable: true,
      });
    }
  }
  get big() {
    return this._x > 40;
  }
  set shift(d) {
    this._x = this._x * 2 + d;
  }
  churn(rounds, dx) {
    const n = rounds | 0;
    for (let i = 0; i < n; i++) {
      const t = this.x;
      this.x = this.y;
      this.y = t;
      this.shift = dx;
      if (this.big) this.x = 0;
    }
  }
}
class Tagged extends Point {
  constructor(x, y) {
    super(x, y);
    this.tag = 't';
  }
}
function step(list, dx) {
  const n = list.length | 0;
  for (let i = 0; i < n; i++) {
    const p = list[i];
    const t = p.x;
    p.x = p.y;
    p.y = t;
    p.shift = dx;
    if (p.big) p.x = 0;
  }
}
function make(n) {
  const list = [];
  for (let i = 0; i < n; i++) list.push(new Point(i, i * 2));
  return list;
}
function show(name, list) {
  const parts = [];
  for (const p of list) {
    parts.push(p.x + '/' + p.y + '/' + p.writes + (p.grown === undefined ? '' : '+' + p.grown) + (p.tag || ''));
  }
  console.log(name, parts.join(' '));
}
function both(name, rounds, dx, prepare) {
  let list = make(8);
  if (prepare) prepare(list);
  for (let r = 0; r < rounds; r++) step(list, dx);
  show(name + ' step', list);
  list = make(8);
  if (prepare) prepare(list);
  for (const p of list) Point.prototype.churn.call(p, rounds, dx);
  show(name + ' churn', list);
}

// Every guard holds.
both('same', 4, 1.5);

// A receiver of another class, and one that is no Point at all.
both('mixed', 4, 2, (list) => {
  list[3] = new Tagged(30, 3);
  list[5] = { x: 7, y: 9, writes: 'none' };
});

// An own data property in front of the accessor from the start.
both('shadowed', 4, 1, (list) => {
  Object.defineProperty(list[2], 'x', { value: 5, writable: true, enumerable: true, configurable: true });
  Object.defineProperty(list[6], 'y', { value: 11, writable: true, enumerable: true, configurable: true });
});

// A setter that puts one there, between two guards on its receiver.
shadowAt = 3;
both('shadowing', 4, 1);
shadowAt = -1;

// A setter that adds a property to its receiver, between two guards on it.
growAt = 6;
both('grown', 4, 3);
growAt = -1;

// A setter that redefines the accessor on the prototype part way through.
redefineAt = 4;
both('redefined', 4, 1);
redefineAt = -1;
both('after', 2, 2);
