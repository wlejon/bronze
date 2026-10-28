// A loop that rotates 40 bindings, one of them fed by a call whose return
// type is dynamic (id is called with strings and objects too): each round
// of the loop fixpoint widens one more binding, so it takes ~40 rounds to
// settle, where a fixed bound of a few rounds used to reject the program.
function id(x){return x;}
function rot(m){ let v0 = 1, v1 = 2, v2 = 3, v3 = 4, v4 = 5, v5 = 6, v6 = 7, v7 = 8, v8 = 9, v9 = 10, v10 = 11, v11 = 12, v12 = 13, v13 = 14, v14 = 15, v15 = 16, v16 = 17, v17 = 18, v18 = 19, v19 = 20, v20 = 21, v21 = 22, v22 = 23, v23 = 24, v24 = 25, v25 = 26, v26 = 27, v27 = 28, v28 = 29, v29 = 30, v30 = 31, v31 = 32, v32 = 33, v33 = 34, v34 = 35, v35 = 36, v36 = 37, v37 = 38, v38 = 39, v39 = 40;
 for (let r = 0; r < m; r++) { const t = v0; v0 = v1; v1 = v2; v2 = v3; v3 = v4; v4 = v5; v5 = v6; v6 = v7; v7 = v8; v8 = v9; v9 = v10; v10 = v11; v11 = v12; v12 = v13; v13 = v14; v14 = v15; v15 = v16; v16 = v17; v17 = v18; v18 = v19; v19 = v20; v20 = v21; v21 = v22; v22 = v23; v23 = v24; v24 = v25; v25 = v26; v26 = v27; v27 = v28; v28 = v29; v29 = v30; v30 = v31; v31 = v32; v32 = v33; v33 = v34; v34 = v35; v35 = v36; v36 = v37; v37 = v38; v38 = v39; v39 = id(t);
  if (r % 3 === 0) { const s = v0; v0 = v39; v39 = s; } }
 return [v0,v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,v11,v12,v13,v14,v15,v16,v17,v18,v19,v20,v21,v22,v23,v24,v25,v26,v27,v28,v29,v30,v31,v32,v33,v34,v35,v36,v37,v38,v39].join(); }
console.log(rot(5), rot(77)); console.log(id('s'), id({}) !== null);
