// `import './side.js'` binds nothing; the module exists only to be
// evaluated, and it is evaluated once, before the importer's body runs.
console.log('side effect');
