// A String exotic object (10.4.3) has TWO sources of own keys: the characters
// of its [[StringData]] plus `length`, and an ordinary shape holding whatever
// else was defined on it. 10.4.3.3 [[OwnPropertyKeys]] reports both, in that
// order, and 10.4.3.1 [[GetOwnProperty]] step 3 hands a key the characters do
// not answer to OrdinaryGetOwnProperty.
//
// bronze answered only the first half. That was invisible for `new String("ab")`
// until something was assigned to it — and catastrophic for `String.prototype`,
// which IS a String exotic object with [[StringData]] "" (22.1.3) and keeps
// every string method in its shape. `Object.getOwnPropertyNames(String.prototype)`
// was `["length"]` while `String.prototype.hasOwnProperty("slice")` was true:
// two members of the same object disagreeing about what it holds, which is what
// breaks a reflective walk over the builtins.
//
// A PRIMITIVE string is the other half of the rule and is pinned beside it: it
// has no shape, so the characters really are its whole answer.
const names = Object.getOwnPropertyNames(String.prototype);
console.log(names.includes("length"), names.includes("slice"), names.includes("indexOf"));
console.log(names.includes("constructor"), names.length > 20);
console.log(Object.hasOwn(String.prototype, "slice"), String.prototype.hasOwnProperty("slice"));
console.log(typeof Object.getOwnPropertyDescriptor(String.prototype, "slice"));
console.log(Object.getOwnPropertyDescriptor(String.prototype, "slice").enumerable);
console.log(Object.getOwnPropertyDescriptor(String.prototype, "length").value);
// Every method is non-enumerable, so the enumerable half of the walk is empty.
console.log(Object.keys(String.prototype).length);
console.log(Reflect.ownKeys(String.prototype).length === names.length + 1);
// `getOwnPropertyDescriptors` loops over the same list, so it grew with it.
console.log(Object.keys(Object.getOwnPropertyDescriptors(String.prototype)).length === names.length);

// A wrapper with an expando: characters, then `length`, then the shape's keys.
const boxed = new String("ab");
boxed.tag = "t";
Object.defineProperty(boxed, "hidden", { value: 1, enumerable: false });
console.log(JSON.stringify(Object.getOwnPropertyNames(boxed)));
console.log(JSON.stringify(Object.keys(boxed)));
console.log(Object.hasOwn(boxed, "tag"), Object.hasOwn(boxed, "hidden"), Object.hasOwn(boxed, "nope"));
console.log(JSON.stringify(Object.getOwnPropertyDescriptor(boxed, "tag")));
console.log(JSON.stringify(Object.getOwnPropertyDescriptor(boxed, 0)));
console.log(String(Object.getOwnPropertyDescriptor(boxed, "nope")));

// The primitive: characters and `length`, and nothing a shape could add.
console.log(JSON.stringify(Object.getOwnPropertyNames("ab")));
console.log(JSON.stringify(Object.keys("ab")));
console.log(Object.hasOwn("ab", "0"), Object.hasOwn("ab", "length"), Object.hasOwn("ab", "slice"));
console.log(String(Object.getOwnPropertyDescriptor("ab", "slice")));

// The other primitive prototypes are ordinary objects and always listed their
// members; pinned here so the string arm cannot drift away from them again.
console.log(Object.getOwnPropertyNames(Number.prototype).includes("toFixed"));
console.log(Object.getOwnPropertyNames(Boolean.prototype).includes("valueOf"));
console.log(Object.getOwnPropertyNames(Array.prototype).includes("map"));
