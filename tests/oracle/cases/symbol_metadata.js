// Symbol.metadata is provided, so the decorator helpers TypeScript and Lit emit
// find it and leave the frozen Symbol alone (Babylon, model-viewer).
if (typeof Symbol.metadata !== 'symbol') {
  Object.defineProperty(Symbol, 'metadata', { value: Symbol('Symbol.metadata') });
}
"undefined" == typeof Symbol || Symbol.metadata ||
  Object.defineProperty(Symbol, "metadata", { configurable: true, writable: true, value: Symbol("Symbol.metadata") });
var _Symbol;
(_Symbol = Symbol).metadata ?? (_Symbol.metadata = Symbol("metadata"));
console.log(typeof Symbol.metadata, Symbol.metadata.description, Symbol.metadata === Symbol.metadata);
console.log(Symbol.keyFor(Symbol.metadata));
const meta = {};
class A {}
A[Symbol.metadata] = meta;
console.log(A[Symbol.metadata] === meta, Object.getOwnPropertySymbols(A).length);
