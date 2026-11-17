// Blocks a harvested run never entered are laid out cold (never_entered.cpp
// unenteredBlocks) and the inliner skips their calls; a later run that does
// enter them, through every copy, must answer the same.
function scale(v, k) { return v * k; }
function label(o) { return o.name + ":" + o.n; }

function step(arr, mode) {
    let sum = 0;
    for (let i = 0; i < arr.length; i++) {
        const x = arr[i];
        if (mode === 0) {
            sum += x;
        } else if (mode === 1) {
            sum += scale(x, 3);
            if (x > 2) sum -= scale(1, x);
        } else {
            const o = { name: "m" + mode, n: x };
            sum += label(o).length;
            try {
                if (x === 3) throw new Error("three");
            } catch (e) {
                sum += e.message.length * 100;
            }
        }
    }
    return sum;
}

const data = [1, 2, 3, 4, 5];
let total = 0;
for (let r = 0; r < 2000; r++) total += step(data, 0);
console.log(total);
console.log(step(data, 1));
console.log(step(data, 2));
console.log(step([1.5, 2.5, 3], 1));
console.log(step(["a", "b"], 0));
