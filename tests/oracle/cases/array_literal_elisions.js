// Array literal holes (elisions) created directly in literal syntax.

const a = [1, , 3, , 5];
console.log("Length:", a.length);
console.log("0 in a:", 0 in a);
console.log("1 in a:", 1 in a);
console.log("2 in a:", 2 in a);
console.log("3 in a:", 3 in a);
console.log("4 in a:", 4 in a);
console.log("5 in a:", 5 in a);

console.log("a[1]:", a[1]);
console.log("Keys:", Object.keys(a).join(","));

let seen = "";
a.forEach((val, idx) => {
  seen = seen + idx + ":" + val + ";";
});
console.log("ForEach seen:", seen);

const doubled = a.map((x) => x * 2);
console.log("Doubled length:", doubled.length);
console.log("1 in doubled:", 1 in doubled);
console.log("Doubled values:", doubled[0], doubled[1], doubled[2], doubled[3], doubled[4]);

// Array with leading and trailing holes
const b = [, , 42, ,];
console.log("b length:", b.length);
console.log("0 in b:", 0 in b);
console.log("1 in b:", 1 in b);
console.log("2 in b:", 2 in b);
console.log("3 in b:", 3 in b);
console.log("b[2]:", b[2]);

// Spread iteration over sparse array densifies per spec
const densified = [...a];
console.log("Densified length:", densified.length);
console.log("1 in densified:", 1 in densified);
console.log("Densified [1]:", densified[1]);
