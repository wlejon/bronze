// instanceof with Array, TypedArray, ArrayBuffer, DataView, Object (7.3.22)
console.log([] instanceof Array);
console.log([] instanceof Object);
console.log({} instanceof Array);
console.log({} instanceof Object);

const f32 = new Float32Array(4);
console.log(f32 instanceof Float32Array);
console.log(f32 instanceof Uint8Array);
console.log(f32 instanceof Array);
console.log(f32 instanceof Object);

const u8 = new Uint8Array(2);
console.log(u8 instanceof Uint8Array);
console.log(u8 instanceof Float32Array);
console.log(u8 instanceof Object);

const buf = new ArrayBuffer(16);
console.log(buf instanceof ArrayBuffer);
console.log(buf instanceof Float32Array);
console.log(buf instanceof Object);

const dv = new DataView(buf);
console.log(dv instanceof DataView);
console.log(dv instanceof ArrayBuffer);
console.log(dv instanceof Object);
