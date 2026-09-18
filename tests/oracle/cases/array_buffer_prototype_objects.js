// `ArrayBuffer.prototype`, `SharedArrayBuffer.prototype` and
// `DataView.prototype` as OBJECTS (ECMA-262 25.1, 25.2, 25.3).
//
// Each used to be a C table standing in for the prototype: a buffer had no
// [[Prototype]] a program could read, `ArrayBuffer.prototype` was a named
// refusal, and `class extends ArrayBuffer` kept its prototype on a side box.
// Now each intrinsic is an ordinary object chained to `Object.prototype`,
// every instance is an ordinary object with internal slots whose
// [[Prototype]] comes from NewTarget, and the members are found by the walk
// every other object takes. So the case pins what a program can now see and
// could not before: the prototypes' identities and sorted own-name lists, the
// accessors and their descriptors, name/length, `isView` and `@@species` as
// own properties of the constructor, a wrong receiver being a TypeError
// rather than a crash, subclassing, and the ordinary object machinery —
// expandos, `Object.keys`, JSON, freeze, `setPrototypeOf` — treating a
// buffer as the object it is.

const names = (o) => Object.getOwnPropertyNames(o).sort().join(",");
const desc = (o, k) => {
  const d = Object.getOwnPropertyDescriptor(o, k);
  return d === undefined
    ? "absent"
    : (d.get ? "get:" + d.get.name + "/set:" + typeof d.set : "value:" + typeof d.value) +
        " w:" + d.writable + " e:" + d.enumerable + " c:" + d.configurable;
};
const fails = (f) => {
  try {
    f();
    return "no throw";
  } catch (e) {
    return (e instanceof TypeError) + " " + e.name;
  }
};

// --- ArrayBuffer (25.1) -------------------------------------------------------
const ab = new ArrayBuffer(8);
const ABP = ArrayBuffer.prototype;
console.log(Object.getPrototypeOf(ab) === ABP, ab instanceof ArrayBuffer, ABP.constructor === ArrayBuffer, Object.getPrototypeOf(ABP) === Object.prototype, Object.getPrototypeOf(ArrayBuffer) === Function.prototype);
console.log(names(ABP));
console.log(names(ArrayBuffer), ArrayBuffer.name, ArrayBuffer.length, ArrayBuffer.isView.length, ArrayBuffer.isView.name);
console.log(Object.prototype.toString.call(ab), Object.prototype.toString.call(ABP), ABP[Symbol.toStringTag], desc(ABP, Symbol.toStringTag));
console.log(desc(ABP, "byteLength"), desc(ABP, "maxByteLength"), desc(ABP, "resizable"), desc(ABP, "detached"));
console.log(desc(ABP, "slice"), desc(ABP, "resize"), desc(ABP, "transfer"), desc(ABP, "transferToFixedLength"), desc(ABP, "constructor"));
console.log(desc(ArrayBuffer, "isView"), desc(ArrayBuffer, Symbol.species), desc(ArrayBuffer, "prototype"), ArrayBuffer[Symbol.species] === ArrayBuffer, Object.getOwnPropertySymbols(ArrayBuffer).map(String).join(","));
console.log(ABP.slice.name, ABP.slice.length, ABP.resize.length, ABP.transfer.length, ABP.transferToFixedLength.length, Object.getOwnPropertyDescriptor(ABP, "byteLength").get.name, Object.getOwnPropertyDescriptor(ABP, "byteLength").get.length);
console.log(ArrayBuffer.isView(new Uint8Array(1)), ArrayBuffer.isView(new DataView(ab)), ArrayBuffer.isView(ab), ArrayBuffer.isView([]), ArrayBuffer.isView(), ArrayBuffer.isView.call(null, new Int8Array(1)));
console.log(Object.getOwnPropertyDescriptor(ABP, "byteLength").get.call(ab), Object.getOwnPropertyDescriptor(ABP, "resizable").get.call(ab), Object.getOwnPropertyDescriptor(ABP, "maxByteLength").get.call(new ArrayBuffer(2, { maxByteLength: 16 })), Object.getOwnPropertyDescriptor(ABP, "detached").get.call(ab));
console.log(ABP.slice.call(ab, 2, 6).byteLength, ABP.slice.call(ab, 2, 6) instanceof ArrayBuffer, ABP.slice.call(ab).byteLength);
console.log(fails(() => Object.getOwnPropertyDescriptor(ABP, "byteLength").get.call({})), fails(() => ABP.byteLength), fails(() => ABP.slice.call(new Uint8Array(1), 0)), fails(() => ABP.slice.call(new SharedArrayBuffer(1))), fails(() => ABP.resize.call(new DataView(ab), 1)), fails(() => ABP.transfer.call({})));
console.log(fails(() => Object.getOwnPropertyDescriptor(ABP, "detached").get.call(new SharedArrayBuffer(1))), fails(() => Object.getOwnPropertyDescriptor(ABP, "resizable").get.call(undefined)), fails(() => ArrayBuffer(8)), fails(() => ArrayBuffer.call({}, 8)));
console.log(ab.hasOwnProperty("byteLength"), "byteLength" in ab, "slice" in ab, ab.hasOwnProperty("slice"), Object.getOwnPropertyNames(ab).length, Object.keys(ab).length);
ab.tag = "expando";
console.log(ab.tag, Object.keys(ab).join(","), names(ab), JSON.stringify(ab), JSON.stringify({ ab }), JSON.stringify(new ArrayBuffer(2)));
Object.defineProperty(ab, "hidden", { value: 3, enumerable: false });
console.log(names(ab), ab.hidden, Object.getOwnPropertyDescriptor(ab, "hidden").enumerable, delete ab.tag, ab.tag);
let forIn = "";
for (const k in ab) forIn += k + ";";
console.log(forIn, JSON.stringify({ ...ab }), Reflect.ownKeys(new ArrayBuffer(1)).length);
const fr = Object.freeze(new ArrayBuffer(4));
console.log(Object.isFrozen(fr), Object.isExtensible(fr), fr.byteLength, fr.slice(1).byteLength, Object.isSealed(Object.seal(new ArrayBuffer(1))));
const detached = new ArrayBuffer(4);
Object.setPrototypeOf(detached, null);
console.log(Object.getPrototypeOf(detached), detached.slice, Object.getOwnPropertyDescriptor(ABP, "byteLength").get.call(detached), ABP.slice.call(detached, 1).byteLength, new Uint8Array(detached).length);
const adopted = Object.create(ABP);
console.log(adopted instanceof ArrayBuffer, typeof adopted.slice, fails(() => adopted.slice(0)), fails(() => adopted.byteLength), fails(() => new Uint8Array(adopted)));
class Buf extends ArrayBuffer {
  get half() {
    return this.byteLength / 2;
  }
}
const b = new Buf(6);
console.log(Object.getPrototypeOf(b) === Buf.prototype, b instanceof Buf, b instanceof ArrayBuffer, b.byteLength, b.half, Object.getPrototypeOf(Buf) === ArrayBuffer, Buf.isView === ArrayBuffer.isView, Buf[Symbol.species] === Buf);
console.log(b.slice(2) instanceof Buf, b.slice(2).half, new Uint8Array(b).length, new Uint8Array(b, 2).buffer === b, Object.prototype.toString.call(b), JSON.stringify(b));
class PlainBuf extends ArrayBuffer {
  static get [Symbol.species]() {
    return ArrayBuffer;
  }
}
console.log(new PlainBuf(2).slice(0) instanceof PlainBuf, new PlainBuf(2).slice(0) instanceof ArrayBuffer, new PlainBuf(2).slice(0).constructor === ArrayBuffer);
function Other() {}
const viaOther = Reflect.construct(ArrayBuffer, [3], Other);
console.log(Object.getPrototypeOf(viaOther) === Other.prototype, viaOther instanceof ArrayBuffer, Object.getOwnPropertyDescriptor(ABP, "byteLength").get.call(viaOther), fails(() => viaOther.byteLength), new Uint8Array(viaOther).length, ArrayBuffer.isView(viaOther));
const rz = new ArrayBuffer(4, { maxByteLength: 8 });
rz.resize(6);
console.log(rz.byteLength, rz.resizable, rz.maxByteLength, fails(() => ABP.resize.call(new ArrayBuffer(1), 2)), rz.transfer().byteLength, rz.detached);
console.log(ABP.slice.call(new Buf(4), 1).constructor === Buf, ABP.transfer.call(new Buf(4)) instanceof Buf, ABP.transfer.call(new Buf(4)).constructor === ArrayBuffer);

// --- SharedArrayBuffer (25.2) --------------------------------------------------
const sab = new SharedArrayBuffer(8);
const SABP = SharedArrayBuffer.prototype;
console.log(Object.getPrototypeOf(sab) === SABP, sab instanceof SharedArrayBuffer, sab instanceof ArrayBuffer, SABP.constructor === SharedArrayBuffer, Object.getPrototypeOf(SABP) === Object.prototype);
console.log(names(SABP), names(SharedArrayBuffer), SharedArrayBuffer.name, SharedArrayBuffer.length);
console.log(Object.prototype.toString.call(sab), SABP[Symbol.toStringTag], desc(SABP, Symbol.toStringTag), desc(SABP, "byteLength"), desc(SABP, "growable"), desc(SABP, "maxByteLength"), desc(SABP, "grow"), desc(SABP, "slice"));
console.log(desc(SharedArrayBuffer, Symbol.species), SharedArrayBuffer[Symbol.species] === SharedArrayBuffer, desc(SharedArrayBuffer, "prototype"), SABP.slice.length, SABP.grow.length, SABP.slice === ABP.slice);
console.log(Object.getOwnPropertyDescriptor(SABP, "byteLength").get.call(sab), Object.getOwnPropertyDescriptor(SABP, "growable").get.call(sab), SABP.slice.call(sab, 2).byteLength, SABP.slice.call(sab, 2) instanceof SharedArrayBuffer);
console.log(fails(() => Object.getOwnPropertyDescriptor(SABP, "byteLength").get.call(ab)), fails(() => SABP.slice.call(ab)), fails(() => SABP.grow.call(ab, 1)), fails(() => ABP.slice.call(sab)), fails(() => SharedArrayBuffer(1)), fails(() => SABP.byteLength));
const gs = new SharedArrayBuffer(2, { maxByteLength: 8 });
gs.grow(4);
console.log(gs.byteLength, gs.growable, gs.maxByteLength, fails(() => SABP.grow.call(sab, 9)), "grow" in sab, sab.hasOwnProperty("grow"));
class Shared extends SharedArrayBuffer {
  get words() {
    return this.byteLength / 4;
  }
}
const sh = new Shared(8);
console.log(sh instanceof Shared, sh instanceof SharedArrayBuffer, sh.words, sh.slice(4) instanceof Shared, Object.getPrototypeOf(Shared) === SharedArrayBuffer, Object.prototype.toString.call(sh));
sab.note = 1;
console.log(Object.keys(sab).join(","), JSON.stringify(sab), Object.isFrozen(Object.freeze(new SharedArrayBuffer(1))));

// --- DataView (25.3) ----------------------------------------------------------
const dv = new DataView(new ArrayBuffer(16), 4, 8);
const DVP = DataView.prototype;
console.log(Object.getPrototypeOf(dv) === DVP, dv instanceof DataView, DVP.constructor === DataView, Object.getPrototypeOf(DVP) === Object.prototype, Object.getPrototypeOf(DataView) === Function.prototype);
console.log(names(DVP));
console.log(names(DataView), DataView.name, DataView.length, desc(DataView, "prototype"), desc(DataView, Symbol.species), Object.getOwnPropertySymbols(DataView).length);
console.log(Object.prototype.toString.call(dv), Object.prototype.toString.call(DVP), DVP[Symbol.toStringTag], desc(DVP, Symbol.toStringTag));
console.log(desc(DVP, "buffer"), desc(DVP, "byteLength"), desc(DVP, "byteOffset"), desc(DVP, "getInt8"), desc(DVP, "setFloat64"), desc(DVP, "constructor"));
const widths = ["Int8", "Uint8", "Int16", "Uint16", "Int32", "Uint32", "Float32", "Float64", "BigInt64", "BigUint64"];
console.log(widths.map((w) => DVP["get" + w].name + ":" + DVP["get" + w].length + "/" + DVP["set" + w].name + ":" + DVP["set" + w].length).join(" "));
console.log(typeof DVP.getFloat16, typeof DVP.setFloat16, typeof DVP.getInt24);
console.log(Object.getOwnPropertyDescriptor(DVP, "byteLength").get.name, Object.getOwnPropertyDescriptor(DVP, "buffer").get.length, Object.getOwnPropertyDescriptor(DVP, "byteOffset").set);
console.log(Object.getOwnPropertyDescriptor(DVP, "byteLength").get.call(dv), Object.getOwnPropertyDescriptor(DVP, "byteOffset").get.call(dv), Object.getOwnPropertyDescriptor(DVP, "buffer").get.call(dv) === dv.buffer, dv.buffer.byteLength);
DVP.setUint16.call(dv, 0, 258);
console.log(DVP.getUint16.call(dv, 0), DVP.getUint8.call(dv, 0), DVP.getUint8.call(dv, 1), new Uint8Array(dv.buffer)[4], DVP.getUint16.call(dv, 0, true));
console.log(fails(() => DVP.getUint8.call(ab, 0)), fails(() => DVP.getUint8.call(new Uint8Array(1), 0)), fails(() => DVP.setUint8.call({}, 0, 1)), fails(() => Object.getOwnPropertyDescriptor(DVP, "byteLength").get.call(ab)), fails(() => DVP.byteLength), fails(() => DVP.buffer));
console.log(fails(() => DataView(ab)), fails(() => DataView.call({}, ab)), fails(() => new DataView(new Uint8Array(1))), fails(() => new DataView({})), fails(() => DVP.getUint8.call(Object.create(DVP), 0)));
console.log(dv.hasOwnProperty("byteLength"), "byteLength" in dv, "getInt8" in dv, dv.hasOwnProperty("getInt8"), Object.getOwnPropertyNames(dv).length);
dv.tag = "expando";
console.log(dv.tag, Object.keys(dv).join(","), JSON.stringify(dv), JSON.stringify(new DataView(new ArrayBuffer(1))), delete dv.tag, Object.keys(dv).length);
class Reader extends DataView {
  u8(i) {
    return this.getUint8(i);
  }
}
const rd = new Reader(new ArrayBuffer(4), 1);
rd.setUint8(0, 200);
console.log(rd instanceof Reader, rd instanceof DataView, rd.u8(0), rd.byteOffset, rd.byteLength, Object.getPrototypeOf(Reader) === DataView, Object.getPrototypeOf(rd) === Reader.prototype, Object.prototype.toString.call(rd));
const dvOther = Reflect.construct(DataView, [new ArrayBuffer(2)], Other);
console.log(Object.getPrototypeOf(dvOther) === Other.prototype, dvOther instanceof DataView, DVP.getUint8.call(dvOther, 0), fails(() => dvOther.getUint8(0)), Object.getOwnPropertyDescriptor(DVP, "byteLength").get.call(dvOther), ArrayBuffer.isView(dvOther));
const dvDetached = new DataView(new ArrayBuffer(2));
Object.setPrototypeOf(dvDetached, null);
console.log(Object.getPrototypeOf(dvDetached), dvDetached.getUint8, DVP.getUint8.call(dvDetached, 0), Object.getOwnPropertyDescriptor(DVP, "byteLength").get.call(dvDetached));
console.log(Object.isFrozen(Object.freeze(new DataView(new ArrayBuffer(1)))), DVP.getUint8.call(Object.freeze(new DataView(new ArrayBuffer(1))), 0), Object.isExtensible(Object.preventExtensions(new DataView(new ArrayBuffer(1)))));
DVP.peek = function () { return this.getUint8(0); };
console.log(new DataView(new ArrayBuffer(1)).peek(), rd.peek(), names(DVP).split(",").length);
delete DVP.peek;
console.log(typeof rd.peek, names(DVP).split(",").length);
