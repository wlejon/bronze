---
name: Bug report
about: bronze compiles something incorrectly, crashes, or produces wrong output
labels: bug
---

**JavaScript that reproduces it** (smallest program you can manage):

```js
```

**What it should print** (what the spec / a browser says):

```
```

**What bronze does instead** (wrong output, compile error, crash — paste it):

```
```

**Does `--no-infer` change it?**
`bronze build --no-infer` on the same program: same wrong output / correct output / didn't try.
(This tells us immediately whether type inference or the dynamic runtime is at fault.)

**Environment:**
- bronze version (`bronze version`) or nightly date:
- OS / platform:
