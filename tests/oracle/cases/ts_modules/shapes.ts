import type { Pair } from "./types.ts";

export enum Kind { Circle = "circle", Square = "square" }

export namespace Area {
    export let calls = 0;
    export function of(kind: Kind, size: number): number {
        calls++;
        return kind === Kind.Circle ? Math.round(Math.PI * size * size) : size * size;
    }
}

export class Shape {
    constructor(public readonly kind: Kind, private size: number) {}
    area(): number { return Area.of(this.kind, this.size); }
    dims(): Pair { return [this.size, this.size]; }
}
