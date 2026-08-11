// A function held as a VALUE, called through the uniform dynamic convention
// rather than as a direct call: bound to a name, stored on an object, and
// passed as an argument (docs/0007).

const fn = function(a, b) {
    return a + b;
};
const fn_obj = { f: fn };
console.log(fn_obj.f(10, 20));
