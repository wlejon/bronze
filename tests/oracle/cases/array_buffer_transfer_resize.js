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

// Mid-loop detach via valueOf(), setter, Proxy trap, and ArrayBuffer.prototype.resize(0)
{
  const b1 = new ArrayBuffer(64);
  const a1 = new Uint8Array(b1);
  for (let i = 0; i < 64; i++) a1[i] = 1;
  let detached1 = false;
  const evil1 = {
    valueOf() {
      if (!detached1) {
        detached1 = true;
        b1.transfer();
      }
      return 0;
    }
  };
  function sumCoerce(evil) {
    let s = 0;
    let iters = 0;
    for (let i = 0; i < a1.length; i++) {
      iters++;
      if (i === 0) +evil;
      s += a1[i];
    }
    return [Number.isNaN(s), iters, a1.length];
  }
  console.log(sumCoerce(0).join(","));
  console.log(sumCoerce(evil1).join(","));
}

{
  const rab = new ArrayBuffer(64, { maxByteLength: 128 });
  const a2 = new Uint8Array(rab);
  for (let i = 0; i < 64; i++) a2[i] = 2;
  let shrunk = false;
  const evil2 = {
    valueOf() {
      if (!shrunk) {
        shrunk = true;
        rab.resize(0);
      }
      return 0;
    }
  };
  function sumResize(evil) {
    let s = 0;
    let iters = 0;
    for (let i = 0; i < a2.length; i++) {
      iters++;
      if (i === 2) +evil;
      s += a2[i];
    }
    return [Number.isNaN(s), iters, a2.length];
  }
  console.log(sumResize(evil2).join(","));
}

