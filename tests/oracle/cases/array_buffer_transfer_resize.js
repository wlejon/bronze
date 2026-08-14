const buf = new ArrayBuffer(8, { maxByteLength: 16 });
console.log(buf.byteLength);
console.log(buf.maxByteLength);
console.log(buf.resizable);
console.log(buf.detached);

buf.resize(12);
console.log(buf.byteLength);

const u8 = new Uint8Array(buf);
u8[0] = 42;
u8[11] = 99;
console.log(u8[0]);
console.log(u8[11]);

const transferred = buf.transfer();
console.log(buf.detached);
console.log(transferred.byteLength);
console.log(transferred.maxByteLength);
console.log(transferred.resizable);

const u8Transferred = new Uint8Array(transferred);
console.log(u8Transferred[0]);
console.log(u8Transferred[11]);

const fixed = transferred.transferToFixedLength(10);
console.log(transferred.detached);
console.log(fixed.byteLength);
console.log(fixed.maxByteLength);
console.log(fixed.resizable);

const sliced = fixed.slice(0, 4);
console.log(sliced.byteLength);
const u8Sliced = new Uint8Array(sliced);
console.log(u8Sliced[0]);
