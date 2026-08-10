const obj = { a: 1, b: 2 };

function step(o) {
  return o.a + o.b;
}

function loop10(o) {
  return step(o) + step(o) + step(o) + step(o) + step(o) +
         step(o) + step(o) + step(o) + step(o) + step(o);
}

function loop100(o) {
  return loop10(o) + loop10(o) + loop10(o) + loop10(o) + loop10(o) +
         loop10(o) + loop10(o) + loop10(o) + loop10(o) + loop10(o);
}

function loop1000(o) {
  return loop100(o) + loop100(o) + loop100(o) + loop100(o) + loop100(o) +
         loop100(o) + loop100(o) + loop100(o) + loop100(o) + loop100(o);
}

function loop10000(o) {
  return loop1000(o) + loop1000(o) + loop1000(o) + loop1000(o) + loop1000(o) +
         loop1000(o) + loop1000(o) + loop1000(o) + loop1000(o) + loop1000(o);
}

function loop100000(o) {
  return loop10000(o) + loop10000(o) + loop10000(o) + loop10000(o) + loop10000(o) +
         loop10000(o) + loop10000(o) + loop10000(o) + loop10000(o) + loop10000(o);
}

function loop1000000(o) {
  return loop100000(o) + loop100000(o) + loop100000(o) + loop100000(o) + loop100000(o) +
         loop100000(o) + loop100000(o) + loop100000(o) + loop100000(o) + loop100000(o);
}

console.log(loop1000000(obj));
