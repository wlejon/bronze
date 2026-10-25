// Types only, plus one value: what `import type` and inline `type`
// specifiers in main.ts read from.
export interface Foo { n: number }
export interface Bar { v: number }
export type Pair = [number, number];
export function makeBar(): Bar { return { v: 9 }; }
