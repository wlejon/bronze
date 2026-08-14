// toLocaleLowerCase / toLocaleUpperCase answer with the root-locale
// (untailored) mapping. On the ASCII inputs bronze accepts, that is exactly
// the toLowerCase / toUpperCase answer — locale tailorings (22.1.3.26)
// touch only non-ASCII characters, which the runtime refuses loudly.
console.log('AbC'.toLocaleLowerCase());
console.log('AbC'.toLocaleUpperCase());
console.log('mixed 123 Case!'.toLocaleLowerCase());
console.log('mixed 123 Case!'.toLocaleUpperCase());
console.log('AbC'.toLocaleLowerCase() === 'AbC'.toLowerCase());
console.log('AbC'.toLocaleUpperCase() === 'AbC'.toUpperCase());
