import { measure } from './harness.js';

const N = 10000000;
const s = '...';
const re = /a/g;

function bench() {
    let acc = 0;
    for (let i = 0; i < N; i++) {
        acc += s.replace(re, 'b').length;
    }
    return acc;
}

const t = measure('str_replace_10m', bench, N);
console.log('checksum ' + t);
