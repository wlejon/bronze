import { Node } from './base.js';

// One file down: a class that adds fields and a method, no accessor.
export class View extends Node {
    constructor(label) { super(); this.label = label; }
    tag() { return this.label + ':' + this.describe(); }
}

// Same source spelling as a class in main.js.
export class Leaf extends View {
    constructor() { super('mid-leaf'); }
    get x() { return 1000 + this.position.x; }
}
