// Facts about an array (its length, its being a dense array, its elements)
// held across code that changes it through another name: a callee pushing
// onto the same array, an alias popping, a fresh array allocated in the loop
// that must not be mistaken for the old one, and length set from outside.
function grow(arr, v) { arr.push(v); }
function shrink(holder) { holder.list.pop(); }
function walk(arr, holder, mode) {
  let s = 0;
  for (let i = 0; i < arr.length; i++) {
    s += arr[i] | 0;
    if (mode === 1 && i === 2) grow(arr, 100 + i);
    if (mode === 2 && i === 1) shrink(holder);
    if (mode === 3 && i === 0) holder.list.length = 3;
    if (mode === 4 && i === 1) holder.list[arr.length + 2] = 7;
    const fresh = [i, i + 1];
    fresh[0] = s;
    s += fresh.length + (arr.length > 5 ? 1 : 0);
  }
  return s + ':' + arr.length;
}
let out = '';
for (let r = 0; r < 3000; r++) {
  const a = [1, 2, 3, 4, 5, 6];
  out = walk(a, { list: a }, 0);
}
console.log('warm', out);
for (let mode = 1; mode <= 4; mode++) {
  const a = [1, 2, 3, 4, 5, 6];
  console.log(mode, walk(a, { list: a }, mode), JSON.stringify(a));
}
const b = [1, 2, 3, 4, 5, 6];
console.log(walk(b, { list: [9] }, 2), walk(b, { list: b.slice() }, 1), b.length);
