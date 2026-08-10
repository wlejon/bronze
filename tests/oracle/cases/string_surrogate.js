// Blocked: requires compiler string literal and surrogate pair support

const emoji = "🌍";
const greeting = "Hello " + emoji;

console.log(emoji.length);
console.log(emoji.charCodeAt(0));
console.log(emoji.charCodeAt(1));
console.log(greeting.length);
console.log(greeting.charCodeAt(6));
console.log(greeting.charCodeAt(7));
