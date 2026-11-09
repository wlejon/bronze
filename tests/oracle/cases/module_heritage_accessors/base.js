// The root of a heritage chain declared in another file: its accessors and
// methods are what a guess on a leaf class two files away resolves to.
export class Point {
    constructor(x, y) { this._x = x; this._y = y; this.writes = 0; }
    get x() { return this._x; }
    set x(v) { this._x = v; this.writes++; }
    get y() { return this._y; }
    set y(v) { this._y = v; this.writes++; }
    norm() { return Math.abs(this._x) + Math.abs(this._y); }
}

export class Node {
    constructor() { this.position = new Point(0, 0); this.ticks = 0; }
    get x() { return this.position.x; }
    set x(v) { this.position.x = v; this.ticks++; }
    get y() { return this.position.y; }
    set y(v) { this.position.y = v; this.ticks++; }
    describe() { return 'node(' + this.x + ',' + this.y + ')'; }
}
