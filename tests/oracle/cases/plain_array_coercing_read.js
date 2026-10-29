// A read of a plain Array's element that feeds arithmetic (`a[i] * 2`) is
// lowered, under inference, as a typed element read of kind
// kElemKindPlainArrayF64 (src/il/il_pin.h): ToNumber of the element in one op.
// A Number is its own bits; every other element must be coerced exactly as the
// generic read followed by `*` would coerce it — not read as a double.

function twice(a, i) {
	return a[i] * 2;
}

function square(a, i) {
	return a[i] * a[i];
}

const f = [0.5, 1.5, 2.5];
f[2] = "1";
console.log(f[0] * 2 + f[1] * 2 + f[2] * 2);
f[2] = true;
console.log(f[2] * 2);
f[0] = null;
console.log(f[0] * 2);
f[1] = undefined;
console.log(f[1] * 2);

const g = [1, 2, 3, 4];
console.log(twice(g, 1));
g[1] = "21";
console.log(twice(g, 1));
g[2] = false;
console.log(twice(g, 2));
delete g[3];
console.log(twice(g, 3));
console.log(twice(g, 9));

let calls = 0;
g[0] = {
	valueOf() {
		calls++;
		return 5;
	}
};
console.log(twice(g, 0) + " " + calls);
console.log(square(g, 0) + " " + calls);
g[0] = Symbol("s");
try {
	twice(g, 0);
	console.log("no throw");
} catch (e) {
	console.log(e instanceof TypeError);
}
g[0] = 10n;
try {
	twice(g, 0);
	console.log("no throw");
} catch (e) {
	console.log(e instanceof TypeError);
}

let sum = 0;
const h = [];
for (let i = 0; i < 8; i++) h.push(i % 2 === 0 ? i : String(i));
for (let i = 0; i < 8; i++) sum += h[i] * 1;
console.log(sum);
