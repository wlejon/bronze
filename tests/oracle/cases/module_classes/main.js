import { Shape } from './shape.js';
import { Circle, kind } from './circle.js';
import { kind as baseKind } from './shape.js';

const s = new Shape('plain');
const c = new Circle(2);

console.log(s.describe());
console.log(c.describe());
console.log(c.area());
console.log(kind, baseKind);
console.log(c instanceof Circle, c instanceof Shape);
