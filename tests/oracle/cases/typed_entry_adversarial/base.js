// The base class of a subclass declared in another module (sub.js): a site
// typed on Body may meet a Rocket at run time.
export class Body {
  constructor(x) { this.x = x; this.v = 0; }
  step(dt, drag = 0.5) {
    this.v = this.v + dt * 2 - this.v * drag * dt;
    this.x = this.x + this.v * dt;
    return this;
  }
  energy(scale) { return this.v * this.v * scale; }
}
