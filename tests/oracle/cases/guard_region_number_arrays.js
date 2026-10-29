// NUMBER ARRAYS (src/lower/guard_region.h): a kernel reading a window of
// constant indices off an Array claims the array's NUMBERS bit
// (runtime/array.h) in front of the reads and, when the claim holds, skips the
// per-value number tests. The bit is a cached fact about the elements, so
// every case below is a way to falsify the fact after it was cached — a store
// of something that is not a Number in double form, by each path that can
// store one — followed by a call that must see the new element exactly as the
// language says.
//
// `read16` is the `Matrix4.multiplyMatrices` shape (sixteen declarations, then
// the arithmetic); `dot16` reads two arrays; `Vec.apply12` the read-run shape
// (`Vector3.applyMatrix4`), where each read is consumed as it is made.

function fmt(x) {
	return Object.is(x, -0) ? "-0" : String(x);
}

function read16(e) {
	const a0 = e[0], a1 = e[1], a2 = e[2], a3 = e[3];
	const a4 = e[4], a5 = e[5], a6 = e[6], a7 = e[7];
	const a8 = e[8], a9 = e[9], a10 = e[10], a11 = e[11];
	const a12 = e[12], a13 = e[13], a14 = e[14], a15 = e[15];
	const r0 = a0 * 1, r1 = a1 * 1, r2 = a2 * 1, r3 = a3 * 1, r4 = a4 * 1, r5 = a5 * 1, r6 = a6 * 1, r7 = a7 * 1;
	const r8 = a8 * 1, r9 = a9 * 1, r10 = a10 * 1, r11 = a11 * 1, r12 = a12 * 1, r13 = a13 * 1, r14 = a14 * 1;
	const r15 = a15 * 1;
	return [r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, r13, r14, r15];
}

function dot16(a, b) {
	const a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
	const a4 = a[4], a5 = a[5], a6 = a[6], a7 = a[7];
	const a8 = a[8], a9 = a[9], a10 = a[10], a11 = a[11];
	const a12 = a[12], a13 = a[13], a14 = a[14], a15 = a[15];
	const b0 = b[0], b1 = b[1], b2 = b[2], b3 = b[3];
	const b4 = b[4], b5 = b[5], b6 = b[6], b7 = b[7];
	const b8 = b[8], b9 = b[9], b10 = b[10], b11 = b[11];
	const b12 = b[12], b13 = b[13], b14 = b[14], b15 = b[15];
	return a0 * b0 + a1 * b1 + a2 * b2 + a3 * b3 + a4 * b4 + a5 * b5 + a6 * b6 + a7 * b7 +
		a8 * b8 + a9 * b9 + a10 * b10 + a11 * b11 + a12 * b12 + a13 * b13 + a14 * b14 + a15 * b15;
}

class Vec {
	constructor(x, y, z) {
		this.x = x;
		this.y = y;
		this.z = z;
	}
	apply12(m) {
		const x = this.x, y = this.y, z = this.z;
		const e = m.elements;
		this.x = e[0] * x + e[1] * y + e[2] * z + e[3] * x + e[4] * y + e[5] * z +
			e[6] * x + e[7] * y + e[8] * z + e[9] * x + e[10] * y + e[11] * z;
		return this;
	}
}

function run12(e, x) {
	return new Vec(x, x, x).apply12({ elements: e }).x;
}

function show(label, values) {
	console.log(label + ": " + values.map(fmt).join(" "));
}

function fresh() {
	return [0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5, 10.5, 11.5, 12.5, 13.5, 14.5, 15.5];
}

// Warm every kernel on arrays of Numbers, so the bit is claimed and kept.
let warm = 0;
const ones = [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1];
for (let i = 0; i < 2000; i++) {
	const e = fresh();
	warm += read16(e)[15] + dot16(e, ones) + run12(e, 2);
}
console.log("warm " + warm);

// 1. Special Numbers keep their bits: NaN, -0, both infinities, the largest
//    and smallest doubles, and Int32-boxed values a bitwise operator made.
const special = [NaN, -0, Infinity, -Infinity, 1.7976931348623157e308, 5e-324, (7 | 0), (-3 >> 0),
	0, -0.5, 2 ** 31, -(2 ** 31), 0.1, 1e21, -1e-7, (255 & 0xff)];
show("special", read16(special));
show("special again", read16(special));
console.log("run12 special " + fmt(run12([-0, -0, -0, -0, -0, -0, -0, -0, -0, -0, -0, -0], 1)));

// 2. A non-Number stored after the claim, by each store path.
let e = fresh();
read16(e);
e[3] = "7";
show("string const index", read16(e));

e = fresh();
read16(e);
for (let i = 0; i < 16; i++) if (i === 9) e[i] = { valueOf() { console.log("valueOf ran"); return 90; } };
show("object dynamic index", read16(e));

e = fresh();
read16(e);
e[7] = true;
show("bool", read16(e));

e = fresh();
read16(e);
e[0] = undefined;
e[1] = null;
show("undefined null", read16(e));

e = fresh();
read16(e);
e[2] = String(2.25);
show("computed string", read16(e));

// 3. Holes, and lengths that move.
e = fresh();
read16(e);
delete e[4];
show("hole", read16(e));
console.log("4 in e " + (4 in e));

e = fresh();
read16(e);
e.length = 10;
show("shrunk", read16(e));
e.length = 16;
show("regrown", read16(e));

// pop keeps the bit, so the push behind it is the store that must clear it.
e = fresh();
read16(e);
e.pop();
read16(e);
e.push("15");
show("push string", read16(e));

e = fresh();
e.pop();
e.push(-0);
show("push -0", read16(e));

e = fresh();
read16(e);
e.shift();
show("shift", read16(e));
e.unshift("u");
show("unshift", read16(e));

// 4. Builtins that rewrite elements.
e = fresh();
read16(e);
e.reverse();
show("reverse", read16(e));
e.splice(3, 1, "s");
show("splice", read16(e));
e = fresh();
read16(e);
e.fill("f", 14);
show("fill", read16(e));
e = fresh();
read16(e);
e[0] = "z";
e.copyWithin(5, 0, 1);
show("copyWithin", read16(e));
e = fresh();
e[0] = 100;
e[1] = "20";
read16(e);
e.sort((x, y) => x - y);
show("sort", read16(e));

// 5. A properties object, a defined index, a frozen array.
e = fresh();
read16(e);
e.extra = "x";
show("named property", read16(e));

e = fresh();
read16(e);
// bronze keeps no per-element accessors (builtin_object_define.cpp refuses
// one on an Array index), so a data descriptor is the define that can store.
Object.defineProperty(e, 6, { value: "66" });
show("index define", read16(e));

e = fresh();
read16(e);
Object.freeze(e);
console.log("frozen set " + Reflect.set(e, 1, "no"));
show("frozen", read16(e));

// 6. Two arrays, the same array twice, and an array shared with code that
//    writes it generically between calls.
const a = fresh();
const b = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16];
console.log("dot " + dot16(a, b));
console.log("dot self " + dot16(b, b));
b[15] = "1";
console.log("dot string " + dot16(a, b));
function scribble(arr, i, v) {
	arr[i] = v;
}
for (let i = 0; i < 16; i++) scribble(b, i, 2);
console.log("dot after scribble " + dot16(a, b));
scribble(b, 8, [3]);
console.log("dot array element " + dot16(a, b));

// 7. The read-run shape.
e = fresh();
console.log("run12 " + run12(e, 2));
e[11] = "1";
console.log("run12 string " + run12(e, 2));
e[11] = 11.5;
delete e[0];
console.log("run12 hole " + run12(e, 2));

// 8. Not an Array at all: a plain object with index keys, and a typed array.
const plain = {};
for (let i = 0; i < 16; i++) plain[i] = i;
show("plain object", read16(plain));
show("Float64Array", read16(new Float64Array([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, -0])));

// 9. Long arrays are never claimed, short ones fail the bounds.
const long = [];
for (let i = 0; i < 100; i++) long.push(i + 0.25);
show("long", read16(long));
show("short", read16([1, 2, 3]));

// 10. Allocation between calls, so a collection can move the claimed arrays.
let churn = 0;
const keep = fresh();
for (let i = 0; i < 300; i++) {
	const garbage = [];
	for (let j = 0; j < 50; j++) garbage.push({ j, s: "g" + j });
	churn += read16(keep)[i % 16] + garbage.length;
	if (i === 150) keep[5] = "5.5";
}
console.log("churn " + churn);
