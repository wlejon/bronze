// A method site whose receivers escape (they are handed to code the analysis
// cannot see through) while its prediction still names exactly one callee
// beside the unknown: the site is planned as a direct call to that callee,
// guarded by the site's method cache. Each change below reaches the site by a
// route the prediction does not see (a computed key read out of JSON, a
// receiver JSON made), and each must call what the language calls.
class Plane {
  constructor(c) { this.c = c; }
  distanceToPoint(p) { return p.x + this.c; }
}

function intersects(planes, p) {
  let s = 0;
  for (let i = 0; i < planes.length; i++) {
    s += planes[i].distanceToPoint(p);
  }
  return s;
}

const key = JSON.parse('"distanceToPoint"');
const planes = [];
for (let i = 0; i < 6; i++) planes.push(new Plane(i));
const p = { x: 1 };
let total = 0;
for (let k = 0; k < 200; k++) total += intersects(planes, p);
console.log(total);

// A receiver of another shape that carries its own method.
const own = JSON.parse('{"c": 50}');
own[key] = function (q) { return q.x * 100 + this.c; };
planes.push(own);
console.log(intersects(planes, p));

// An own method on one of the class's instances.
planes[0][key] = function (q) { return -q.x; };
console.log(intersects(planes, p));

// The prototype's method replaced.
Plane.prototype[key] = function (q) { return q.x * 2 + this.c; };
console.log(intersects(planes, p));

// The prototype's method deleted: the remaining instances throw.
delete Plane.prototype[key];
planes.shift();
try {
  intersects(planes, p);
} catch (e) {
  console.log(e instanceof TypeError);
}

// A receiver with no such method at all.
try {
  intersects([JSON.parse("{}")], p);
} catch (e) {
  console.log(e instanceof TypeError);
}
