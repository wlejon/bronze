// Oracle test for array method IC arms and overflow-slot transitions (Chunk 12)

// 1. Array method identity: a.push === a.push === b.push === Array.prototype.push
const arr1 = [1, 2, 3];
const arr2 = [4, 5, 6];
console.log(arr1.push === arr1.push);
console.log(arr1.push === arr2.push);
console.log(arr1.shift === arr2.shift);
console.log(arr1.constructor === arr2.constructor);
console.log(arr1.constructor === Array);

// 2. Method invocations through IC hit
arr1.push(10);
console.log(arr1.join(","));
console.log(arr1.shift());
console.log(arr1.join(","));

// 3. Shadowing method by own named property
arr1.push = 42;
console.log(arr1.push);
console.log(typeof arr1.push);

// 4. Delete own named property restores builtin
console.log(delete arr1.push);
console.log(typeof arr1.push);
arr1.push(99);
console.log(arr1.join(","));

// 5. Shadowing with undefined
arr1.push = undefined;
console.log(arr1.push === undefined);
console.log("push" in arr1);
console.log(arr1.hasOwnProperty("push"));
delete arr1.push;
console.log(typeof arr1.push);

// 6. RegExp match arrays with own named properties (.index, .input)
const re = /(\w+)\s(\w+)/;
const match = re.exec("hello world");
console.log(match.length);
console.log(match[0], match[1], match[2]);
console.log(match.index);
console.log(match.input);
console.log(typeof match.push);
console.log(match.push === Array.prototype.push);

// 7. Arguments object callee
function testArgs(x, y) {
  console.log(arguments.length);
  console.log(arguments.callee === testArgs);
  console.log(arguments[0], arguments[1]);
}
testArgs(100, 200);

// 8. Polymorphic site receiving plain object and array
function getPush(x) {
  return x.push;
}
const plainObj = { push: "custom_push" };
console.log(getPush(plainObj));
console.log(getPush(arr2) === Array.prototype.push);
console.log(getPush(plainObj));
console.log(getPush(arr2) === Array.prototype.push);

// 9. Overflow slot transitions (objects with >4 properties, e.g. 14 properties)
function BigNode(id) {
  this.p0 = id;
  this.p1 = id + 1;
  this.p2 = id + 2;
  this.p3 = id + 3;
  this.p4 = id + 4;
  this.p5 = id + 5;
  this.p6 = id + 6;
  this.p7 = id + 7;
  this.p8 = id + 8;
  this.p9 = id + 9;
  this.p10 = id + 10;
  this.p11 = id + 11;
  this.p12 = id + 12;
  this.p13 = id + 13;
}

const nodes = [];
for (let i = 0; i < 50; ++i) {
  nodes.push(new BigNode(i * 100));
}

console.log(nodes.length);
console.log(nodes[0].p0, nodes[0].p3, nodes[0].p4, nodes[0].p7, nodes[0].p8, nodes[0].p13);
console.log(nodes[49].p0, nodes[49].p3, nodes[49].p4, nodes[49].p7, nodes[49].p8, nodes[49].p13);
