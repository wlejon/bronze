// Array.prototype.sort with callable Proxy comparator

const targetComparator = (a, b) => a - b;
let trapCount = 0;
const proxyComparator = new Proxy(targetComparator, {
  apply(target, thisArg, args) {
    trapCount++;
    return Reflect.apply(target, thisArg, args);
  }
});

const arr = [5, 2, 8, 1, 9, 3, 7, 4, 6];
arr.sort(proxyComparator);
console.log(arr.join(","));
console.log("trapCount > 0:", trapCount > 0);