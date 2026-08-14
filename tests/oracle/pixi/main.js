// The pixi milestone probe: unmodified pixi.js v8.19.0 (pixi.mjs beside this
// file, vendored byte-for-byte) imports, and a scene graph built from its
// public API answers what the library defines it to answer.
//
// Every expectation in main.expected is derived by READING pixi's source,
// never by running bronze or node:
//   - Container.addChild returns its (single) argument and sets `parent`;
//     removeChild nulls it (Container children machinery).
//   - Texture.WHITE is a 1x1 BufferImageSource (its construction site).
//   - toGlobal composes the parent chain's translations: (10,20)+(5,6).
//   - Rectangle.contains is half-open: [x, x+w) by [y, y+h).
//   - Matrix.scale multiplies tx/ty, so translate(3,4).scale(2,2) is
//     a=2,d=2,tx=6,ty=8.
import './setup.mjs';
import { Container, Sprite, Texture, Rectangle, Matrix, Point } from 'pixi.js';

const stage = new Container();
stage.label = 'stage';
console.log('PIXI stage.label=' + stage.label);
console.log('PIXI stage.children.length=' + stage.children.length);

const group = new Container();
group.position.set(10, 20);
const added = stage.addChild(group);
console.log('PIXI addChild-returns-child=' + (added === group));
console.log('PIXI group.parent-is-stage=' + (group.parent === stage));
console.log('PIXI stage.children.length=' + stage.children.length);

const tex = Texture.WHITE;
console.log('PIXI texture.white.width=' + tex.width);
console.log('PIXI texture.white.height=' + tex.height);

const sprite = new Sprite(tex);
sprite.position.set(5, 6);
group.addChild(sprite);
console.log('PIXI sprite.x=' + sprite.x);
console.log('PIXI group.children.length=' + group.children.length);

const global = sprite.toGlobal(new Point(0, 0));
console.log('PIXI sprite.global=' + global.x + ',' + global.y);

const r = new Rectangle(0, 0, 16, 16);
console.log('PIXI rect.contains(8,8)=' + r.contains(8, 8));
console.log('PIXI rect.contains(20,8)=' + r.contains(20, 8));

const m = new Matrix();
m.translate(3, 4).scale(2, 2);
console.log('PIXI matrix=' + m.a + ',' + m.b + ',' + m.c + ',' + m.d + ',' + m.tx + ',' + m.ty);

stage.removeChild(group);
console.log('PIXI after-remove.children.length=' + stage.children.length);
console.log('PIXI group.parent-null=' + (group.parent === null));
console.log('PIXI done=1');
