// Blocked: requires dynamic runtime function pointer invocation (fn.call / FunctionHeader)

const fn = function(a, b) {
    return a + b;
};
const fn_obj = { f: fn };
console.log(fn_obj.f(10, 20));
