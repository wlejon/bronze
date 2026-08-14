const validStr = "Hello, World!";
console.log(validStr.isWellFormed());
console.log(validStr.toWellFormed());

console.log(validStr.normalize());
console.log(validStr.normalize("NFC"));
console.log(validStr.normalize("NFD"));
console.log(validStr.normalize("NFKC"));
console.log(validStr.normalize("NFKD"));

try {
    validStr.normalize("INVALID");
} catch (e) {
    console.log(e.name);
}
