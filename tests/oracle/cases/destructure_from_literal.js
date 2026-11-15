const out = [];
function swap(a, b) { [a, b] = [b, a]; return a * 10 + b; }
function rot(a, b, c) { [a, b, c] = [b, c, a]; return '' + a + b + c; }
function fromSet() { let x, y; [x, y] = new Set([7, 8]); return x + y; }
function short() { let x = 1, y = 2; [x, y] = [5]; return x + ':' + y; }
function str() { let x, y; [x, y] = 'hi'; return x + y; }
function holes() { let x, y; [x, , y] = [1, 2, 3]; return x + y; }
function fib(n) { let a = 0, b = 1; for (let i = 0; i < n; i++) [a, b] = [b, a + b]; return a; }
function gcd(a, b) { while (b) [a, b] = [b, a % b]; return a; }
function sortTwo(arr) { for (let i = 0; i + 1 < arr.length; i++) if (arr[i] > arr[i + 1]) [arr[i], arr[i + 1]] = [arr[i + 1], arr[i]]; return arr.join(); }
let g1 = 1, g2 = 2; [g1, g2] = [g2, g1]; out.push(g1, g2);
function closureVars() { let p = 1, q = 2; const f = () => p * 10 + q; [p, q] = [q, p]; return f(); }
function tdz() { try { [zz, ww] = [1, 2]; let zz, ww; } catch (e) { return e.constructor.name; } }
function nested() { let a, b, c; [a, [b, c]] = [1, [2, 3]]; return a + b + c; }
function value() { let a, b; const r = ([a, b] = [3, 4]); return r.length + a + b; }
function gen() { function* g() { yield 1; yield 2; yield 3; } let a, b; [a, b] = g(); return a + b; }
function typed() { let a, b; [a, b] = new Float32Array([1.5, 2.5]); return a + b; }
function nullish() { try { let a; [a] = null; } catch (e) { return e instanceof TypeError; } }
out.push(swap(1, 2), rot(1, 2, 3), fromSet(), short(), str(), holes(), fib(30), gcd(1071, 462), sortTwo([3, 1, 2]),
  closureVars(), tdz(), nested(), value(), gen(), typed(), nullish());
console.log(out.join('\n'));
