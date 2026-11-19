// Non-configurable accessors defined on an object crossing the dictionary
// threshold (`Shape::kDictionaryThreshold = 1024`) or already in dictionary
// mode must preserve `configurable: false` rather than resetting to `true`.

// 1. Object right below the threshold (1023 data properties -> adding a
// two-slot accessor pushes slotsUsedAfterAdd(2) past 1024 into toDictionary +
// dictDefine).
const o1 = {};
for (let i = 0; i < 1023; i++) {
  o1["p" + i] = i;
}
Object.defineProperty(o1, "acc", {
  get() {
    return 42;
  },
  enumerable: true,
  configurable: false,
});
const d1 = Object.getOwnPropertyDescriptor(o1, "acc");
console.log(d1.enumerable, d1.configurable, typeof d1.get, d1.set === undefined);
console.log(o1.acc);
console.log(delete o1.acc);
console.log(o1.acc);
let threw1 = false;
try {
  Object.defineProperty(o1, "acc", { configurable: true });
} catch (e) {
  threw1 = e instanceof TypeError;
}
console.log(threw1);

// 2. Object already in dictionary mode (1030 properties plus a delete).
const o2 = {};
for (let i = 0; i < 1030; i++) {
  o2["p" + i] = i;
}
delete o2.p0;
Object.defineProperty(o2, "acc", {
  get() {
    return 99;
  },
  enumerable: false,
  configurable: false,
});
const d2 = Object.getOwnPropertyDescriptor(o2, "acc");
console.log(d2.enumerable, d2.configurable, typeof d2.get, d2.set === undefined);
console.log(o2.acc);
console.log(delete o2.acc);
console.log(o2.acc);
let threw2 = false;
try {
  Object.defineProperty(o2, "acc", { configurable: true });
} catch (e) {
  threw2 = e instanceof TypeError;
}
console.log(threw2);
