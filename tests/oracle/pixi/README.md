# Vendored pixi.js bundle

This directory holds the published ESM browser bundle for **pixi.js v8.19.0**, byte-for-byte as published to npm and served via jsdelivr, plus the pixi oracle milestone that compiles it.

## Vendored Assets

### `pixi.mjs`
```
https://cdn.jsdelivr.net/npm/pixi.js@8.19.0/dist/pixi.mjs
sha256  33e343d12bf00ec0eca62032eacd417d1b29b6b2fd34235c69899ecca42e1958
bytes   2087231
```

### `pixi.min.mjs`
```
https://cdn.jsdelivr.net/npm/pixi.js@8.19.0/dist/pixi.min.mjs
sha256  28fefb52eeb15bb3e087533456bafc53e91af70932af4dd046ff2938ec3edd0e
bytes   798434
```

Vendored byte-for-byte as released, never hand-edited; `.gitattributes` pins the whole directory `-text` so a checkout cannot rewrite line endings out from under the hashes. `node_modules/pixi.js/package.json` is bronze's bare-specifier shim (the upward `node_modules` walk in `src/modules/package.cpp`), so `import ... from 'pixi.js'` resolves to the bundle.

## The milestone (`oracle-pixi`, ctest label `pixi`)

`main.js` imports the unmodified bundle and probes a scene graph built from its public API; `main.expected` pins the exact stdout bytes. Both inference modes compile and run, and each built executable is re-run under `BRONZE_GC_STRESS=1` (a whole-library import under every-allocation collection runs for minutes — the harness gives that run its own budget).

Every line of `main.expected` is derived by **reading pixi's source**, never by running bronze or node — the derivations are cited in `main.js`'s header comment.

pixi's import-time code evaluates two browser globals unconditionally (`navigator`, via `isSafari()` at module level, and `Intl`, whose `== null` check still reads the binding). `setup.mjs` defines both on `globalThis` before `pixi.mjs` evaluates — a property of the global object is a global binding — and `host.globals` is the `--host-globals` manifest that admits the two names at compile time. In bro, the host registers the real ones instead; the manifest seam is the same.

## License

Pixi.js is Copyright © 2013-2024 Mat Groves, Chad Engler, MIT licensed.
