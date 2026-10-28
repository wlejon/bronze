// The shape guard's refusal of a receiver that is no object.
//
// A shape-guarded site reads the receiver's header through a pointer it
// derives without asking whether the value is an object at all: a number, a
// boolean, undefined or null reads the header of a stand-in instead, a block
// whose first word is the prototype-mutation epoch (bronze_abi.h,
// BRONZE_ABI_PROTO_EPOCH_*). The epoch is always even and every header odd,
// so the compare alone sends the primitive to the slow path. This case puts
// every kind of receiver through sites warmed on plain objects and arrays,
// with prototype mutations between rounds that move the epoch, so a stand-in
// whose word could pass for a header would answer a primitive's read with an
// object's slot.

function readX(o) {
    return o.x;
}
function readLen(o) {
    return o.length;
}
function readAt(o, i) {
    return o[i];
}
function callM(o) {
    return o.m();
}

const plain = { x: 1, m() { return 'plain'; } };
const other = { x: 2, y: 3, m() { return 'other'; } };
const arr = [10, 20, 30];

const prims = [7, 2.5, -0, NaN, true, false, 'str', undefined, null];

function round(tag) {
    let s = 0;
    for (let i = 0; i < 200; i = i + 1) {
        s = s + readX(plain) + readX(other) + readLen(arr) + readAt(arr, 1);
        s = s + (callM(i % 2 ? plain : other) === 'plain' ? 1 : 0);
    }
    const out = [tag, s];
    for (const p of prims) {
        let x;
        try {
            x = readX(p);
        } catch (e) {
            x = e.constructor.name;
        }
        let len;
        try {
            len = readLen(p);
        } catch (e) {
            len = e.constructor.name;
        }
        let at;
        try {
            at = readAt(p, 0);
        } catch (e) {
            at = e.constructor.name;
        }
        let m;
        try {
            m = callM(p);
        } catch (e) {
            m = e.constructor.name;
        }
        out.push(String(x) + '/' + String(len) + '/' + String(at) + '/' + String(m));
    }
    console.log(out.join(' '));
}

round('start');

// Each mutation moves the epoch; the prototypes are what the primitives'
// reads then find.
Number.prototype.x = 'nx';
round('number.x');
Boolean.prototype.length = 'blen';
round('boolean.length');
String.prototype.m = function () { return 'sm:' + this.length; };
Number.prototype.m = function () { return 'nm'; };
round('m');
for (let k = 0; k < 64; k = k + 1) {
    Object.prototype['p' + k] = k;
}
round('many');
delete Number.prototype.x;
delete Boolean.prototype.length;
round('deleted');
