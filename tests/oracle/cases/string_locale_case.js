// toLocaleLowerCase / toLocaleUpperCase answer with the root-locale
// (untailored) mapping. On ASCII that is exactly the toLowerCase /
// toUpperCase answer — locale tailorings (22.1.3.26) touch only non-ASCII
// characters, and bronze carries none of them: the case tables are generated
// with SpecialCasing.txt's language-tagged lines dropped, so the four members
// share one answer by construction rather than by coincidence.
console.log('AbC'.toLocaleLowerCase());
console.log('AbC'.toLocaleUpperCase());
console.log('mixed 123 Case!'.toLocaleLowerCase());
console.log('mixed 123 Case!'.toLocaleUpperCase());
console.log('AbC'.toLocaleLowerCase() === 'AbC'.toLowerCase());
console.log('AbC'.toLocaleUpperCase() === 'AbC'.toUpperCase());
