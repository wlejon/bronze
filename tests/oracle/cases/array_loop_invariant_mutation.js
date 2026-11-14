// A loop reads an array's length and elements every iteration while
// something else in the loop grows, shrinks or replaces the array: a call,
// an alias, a getter, a setter. Each read must see the array as it is at
// that iteration (a load hoisted out of the loop would see it as it was
// before), and reads across collections must see moved elements.
function sumGrowing(arr, grow) {
  let s = 0;
  for (let i = 0; i < arr.length; i++) {
    s += arr[i];
    if (i === 2) grow(arr);
  }
  return s;
}
function sumShrinking(arr) {
  const alias = arr;
  let s = 0;
  for (let i = 0; i < arr.length; i++) {
    s += arr[i] * (i + 1);
    if (i === 1) alias.length = 4;
    if (i === 3) alias.shift();
  }
  return s;
}
const holder = {
  list: [1, 2, 3, 4, 5, 6],
  get items() { return this.list; },
};
function sumReplaced(h) {
  let s = 0;
  for (let i = 0; i < h.items.length; i++) {
    s += h.items[i];
    if (i === 1) h.list = [10, 20, 30, 40, 50, 60, 70, 80];
  }
  return s;
}
const view = { arr: [1, 1, 1, 1, 1, 1, 1, 1] };
Object.defineProperty(view, "touch", {
  set(v) { for (let k = 0; k < v; k++) this.arr.push(k); this.arr.splice(0, 2); },
});
function sumSetter(v) {
  const a = v.arr;
  let s = 0;
  for (let i = 0; i < a.length; i++) {
    s += a[i];
    if (i === 0) v.touch = 3;
  }
  return s;
}
// Plain number reads in a loop that allocates enough to collect: the
// elements block may move under the loop.
function sumAllocating(nums) {
  let s = 0;
  let keep = [];
  for (let i = 0; i < nums.length; i++) {
    s += nums[i];
    keep.push({ i, pad: new Array(16).fill(i) });
    if (keep.length > 64) keep = [];
  }
  return s + keep.length;
}
function grow(a) { a.push(100, 200); a.unshift(-1); }

const out = [];
for (let r = 0; r < 300; r++) {
  const g = sumGrowing([1, 2, 3, 4, 5], grow);
  const k = sumShrinking([1, 2, 3, 4, 5, 6, 7, 8]);
  holder.list = [1, 2, 3, 4, 5, 6];
  const p = sumReplaced(holder);
  view.arr = [1, 1, 1, 1, 1, 1, 1, 1];
  const t = sumSetter(view);
  const nums = [];
  for (let j = 0; j < 2000; j++) nums.push(j * 0.5);
  const m = sumAllocating(nums);
  if (r === 0 || r === 299) out.push([g, k, p, t, m].join(" "));
}
console.log(out.join("\n"));
