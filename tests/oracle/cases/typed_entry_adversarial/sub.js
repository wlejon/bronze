import { Body } from './base.js';

export class Rocket extends Body {
  constructor(x) { super(x); this.fuel = 10; }
  step(dt, drag) {
    this.fuel = this.fuel - dt;
    this.v = this.v + (typeof drag) .length + dt;
    this.x = this.x - this.v;
    return this;
  }
}
