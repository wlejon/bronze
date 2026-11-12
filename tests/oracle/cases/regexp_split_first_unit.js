// split by a RegExp whose matches can only start at a few code units: the
// scan may skip indices, never pieces.
const show = (a) => console.log(JSON.stringify(a));
show('The quick  brown\tfox\njumps'.split(/\s+/));
show('  lead and trail  '.split(/ +/));
show('a1b22c333d'.split(/\d+/));
show('a1b22c333d'.split(/(\d)+/));
show('one, two;three ,four'.split(/\s*[,;]\s*/));
show('one, two;three ,four'.split(/[,;]/, 2));
show('no separators here'.split(/x/));
show(''.split(/x/));
show(''.split(/(?:)/));
show('abc'.split(/(?:)/));
show('abc'.split(/b*/));
show('AbCaBc'.split(/b/i));
show('line1\r\nline2\nline3\rline4'.split(/\r\n|\n|\r/));
show('x yézéw'.split(/é/));
show('😀-😁-😂'.split(/-/));
show('😀😁'.split(/\ud83d/));
show('😀😁'.split(/(?:)/u));
show('aXbxc'.split(/x/gi));
show('2020-01-02T03:04'.split(/[-T:]/));
show('abcabc'.split(/^a/));
show('ab\nab'.split(/^a/m));
show('a.b.c'.split(/\./, 0));
show('wordAwordBword'.split(/(?=[A-Z])/));
show('tick tock'.split(/(?<=k) /));
const sticky = /,/y;
sticky.lastIndex = 3;
show('a,b,c'.split(sticky));
console.log(sticky.lastIndex);
// The words pixi's text measurer splits on.
show('Hello  world\nnew line'.split(/(?:\r\n|\r|\n)/));
show('Hello  world'.split(/( )/));
