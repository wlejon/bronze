// A character outside the BMP is TWO UTF-16 code units, and every unit-indexed
// operation says so: `length` counts units, and charCodeAt yields the high and
// low surrogate separately.

const emoji = "🌍";
const greeting = "Hello " + emoji;

console.log(emoji.length);
console.log(emoji.charCodeAt(0));
console.log(emoji.charCodeAt(1));
console.log(greeting.length);
console.log(greeting.charCodeAt(6));
console.log(greeting.charCodeAt(7));
