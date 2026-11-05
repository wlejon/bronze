# AOT code size and taken branches

The follow-on to [aot-specialization.md](aot-specialization.md), "Cycle
accounting": that section found bro's frame on three_meshes losing about
1.3 ms a frame to frontend bandwidth (the hot code does not fit the op cache,
so 54% of ops come through the legacy decoder) and ranked hot code size and
taken branches first. This file covers the brass layout and encoding work
done against those two numbers, what it bought, and what it did not.

Machine and method as there: halo (Zen 5), three_meshes n=2565, three_skinned
n=410, three_instanced n=42992, `perf stat -I` counting runs over the steady
window, LBR (`perf record -b`) for taken branches by site, IBS op samples for
the hot set (the 64-byte lines holding 90% of a frame's ops). Variants were
compared round-robin with a rest between runs (sustained GPU load slows halo
by minutes), never one side after the other.

## What the taken branches were

LBR on the base, branches inside app.so functions, named by the IR blocks
they leave and enter (brass's `BRASS_BLOCK_MAP`, below), per frame:

| site | taken | what |
|---|---|---|
| truthy critical edges (`truthy_*` to their merges) | ~470K | the classifier's one-sided arms jump to a merge that the branch then tests again |
| `__bronze_prop_get_ic` (pfast blocks) | ~340K | the stub's object test let its slow call fall through |
| `static_get` merge → `merge_split` | 155K | the read's merge laid out after the miss |
| `recv_N_obj` → `recv_N_plain` (S6) | 154K | the plain-object guard's out-of-line shape test |
| `seq_num` → merge | 149K | strict equality's Number arm |
| `elem_get` obj → array, array_load → merge | ~170K | element reads' Array arm out of line |

Two of these are brass decisions that nothing in bronze can steer, and they
had one cause each:

- **recv_N_plain.** A static guard's miss whose receiver test a dominating
  guard had already made is an empty `br` to the fallback read.
  `cfg_simplify`'s trampoline elimination bypassed it, so the guard's header
  test branched straight to the fallback block, which nothing marked cold,
  and the layout let it fall through with the guard's hit out of line.
- **The IC stub.** The call heuristic (cold side of a two-way branch: the
  one that calls) applied only to a side entered once. The stub's object
  test and shape test both refuse to the same slow call, entered twice, so
  the call fell through and the object case jumped.

## What is built (brass)

1. **A cold trampoline is kept** (`mir/cfg_simplify.cpp`,
   `eliminate_trampolines`). A trampoline the front end names cold (or marks
   `set_cold`) is bypassed only by cold predecessors; a hot predecessor keeps
   branching to it, so the cold mark stays on the edge.
2. **Cold propagates forward** (`codegen/block_layout.cpp`). A block entered
   only from cold blocks is cold too: the code a miss runs (a full IC read's
   own tests, hit and merge) is named for the miss only at its first block.
   A block any warm block enters stays warm, so every merge with the hot path
   stays in the trace.
3. **The call heuristic holds however many branches enter the calling side**
   (`shape_score`): between a side that calls and one that does not, the one
   that does not falls through.
4. **The MIR branch's true side is the likely one.** Where nothing else tells
   the two sides apart, the side the `br_if` names first (or the copy block
   carrying that edge) gets the fall-through bonus that the jump target got
   before. Instruction selection turns a branch either way (a side's copies
   decide which one the `jmp` names), so the jump target said nothing; the
   front end's order does say something (a guard's hit before its miss, an
   Array before a typed array, two Numbers before the generic compare).
5. **Short branches** (`codegen/emit_context.cpp`, `emit_jump`). Every
   block-to-block branch was rel32 (jcc 6 bytes, jmp 5). A backward branch
   is now exact (rel8 where it reaches); forward branches are measured in an
   all-rel32 first pass, those within reach are emitted rel8 in a second
   pass, and any that alignment padding pushed out of reach after all
   (`CodeBuffer::collect_rel8_overflows`) goes long and the function is
   emitted again, at most four times, else the all-rel32 pass stands. All
   five branch sites in emission go through `emit_jump`.
6. **Cold blocks are not aligned.** Blocks nothing falls into were padded to
   16 bytes; a cold one no longer is. (Loop heads keep 32.)
7. **`mov r64, imm` in its shortest form** (`x64_encoder.cpp` `mov(GPR,
   int64)`, `emit_alu.cpp`): `mov r32, imm32` (5 bytes, zero-extends) for
   0..2^32-1, `REX.W C7` (7) for a negative imm32, `movabs` (10) only beyond.
   Non-patchable, non-symbol `Movabs` LIR goes through it.
8. **`BRASS_BLOCK_MAP=<file>`** (`object/object_writer.cpp`), a diagnostic:
   appends `function, offset, block name, cold` per emitted x64 block, for
   naming LBR and IBS addresses by IR block.

Tests: brass `tests/unit/test_layout_cold.cpp`, six cases: the cold
trampoline kept (and a warm one still bypassed), a block only cold blocks
enter laid out with them, a slow call entered from two tests not falling
through, a reaching branch emitted short and landing on its block, short and
long forms run against a C++ reference across loop bodies from a few bytes
to far past rel8's reach, and a cold block starting where the code before it
ends. The first four and the last fail on the old sources. The encoder
goldens in `test_x64_encoder.cpp` changed with item 7.

## Measured

Same session, round-robin, three runs a side (base = brass main at the start
of this work):

| per frame | base | built | |
|---|---|---|---|
| three_meshes cycles (M) | 38.46, 38.50, 37.88 | 37.90, 37.98, 37.87 | -1.4% |
| three_meshes cpu ms | 7.51, 7.49, 7.38 | 7.39, 7.39, 7.37 | -1.3% |
| three_meshes taken branches | 8.48M | 7.03M | -17% |
| three_meshes op-cache misses / accesses | 11.15M / 19.95M | 10.42M / 18.31M | -6.5% / -8.2% |
| three_meshes decoder ops | 65.9M | 67.4M | +2.3% |
| three_meshes instructions | 121.5M | 120.7M | -0.7% |
| three_skinned cycles (M) | 33.59, 33.79, 34.46 | 31.98, 33.10, 32.34 | -4.3% |
| three_skinned cpu ms | 6.48, 6.52, 6.66 | 6.39, 6.39, 6.44 | -2.0% |
| three_skinned taken branches | 7.61M | 6.28M | -17.5% |
| three_instanced cpu ms (once) | 4.74 | 4.74 | libm-bound |
| 90% hot set (IBS) | 154 KB (app.so 112.5) | 149 KB (app.so 107.6) | -3% |
| setProgram bytes / hot | 102,252 / 18.6 KB | 97,459 / 18.1 KB | -4.7% |
| app.so .text (three_meshes) | 20.98 MB | 19.67 MB | -6.2% |
| `bronze build` of three_meshes | ~36.5 s | ~37-38 s | two-pass emission |

Pure-compute benches (`bench/`, min of 5, cycles; instructions unchanged
within 1.6%): object_graph 85.1 → 71.1M (-16%, taken 24.5 → 11.7M),
mesh_churn_2k -1.1%, three_math -0.4% (taken 3.98 → 2.72M),
instanced_mesh_churn +2.0%, the rest within 0.6%.

### What did not pay, and why

Taken branches fell 17% and op-cache accesses 8%, and cycles moved about
1.5% on three_meshes: a taken branch here is cheap. Two reasons show in the
counters:

- **Decoder ops went up** with every denser layout (+2-3%). Packing the
  same hot code into fewer bytes puts more ops in each 64-byte line; a
  likely cause is the op cache's per-line capacity, not confirmed. The hot
  set shrank only 3% because the cold blocks were mostly already off the
  hot lines; what the line count measures is hot code.
- **The layout variants were indistinguishable in cycles.** Measured
  round-robin against the same intermediate build (items 1-3), three
  rounds, two sessions: without the true-side bias (item 4) taken branches
  stay at 7.7M rather than 7.0M at the same cycles; without short branches
  .text is 0.4 MB bigger at the same cycles; aligning no block but loop
  heads saves another 0.85 MB of .text and measured no better than item 6.
  Every variant sat 1.5-2.5% under base and within ±1% of each other.
  Item 4 and item 6 are kept because their combination was best in both
  sessions, not by a margin the noise excludes.

So the frame's frontend loss is not the branches and not the padding: it is
the hot code's own size, which is its instructions and their encodings.

## Taken branches left

LBR on the built version, branches inside app.so functions named by block
(3.05M of 7.03M a frame are named; the rest cross functions or the runtime):

| site | taken |
|---|---|
| truthy merges (`truthy_*_merge_crit` in and out, `notnullish` arms, the merge to its branch) | ~465K |
| loop back edges (`br_if_false` → head, `b` → `b`) | ~220K, expected |
| strict equality merges (`seq_*_merge`, `seq_*_other` crit) | ~140K |
| IC stub ways (`pfast_way` → next way, `pfast_hit` → `pfast_notown`) | ~145K |
| store merges (`static_set` hit → merge, `elem_set` array store → merge → branch) | ~160K |
| `ic_absent` (and-chain → test, undef → merge) | ~100K |

The largest is bronze's lowering, not brass's layout: truthiness computes a
value in two or three arms, merges it, and the branch tests the merged
value. Fusing truthy with the branch it feeds (each arm branching to the
branch's own target) removes the merges and their taken edges.

## Hot bytes left

hotbytes on the base's hot app.so lines (by byte, the 90% set):

| | share |
|---|---|
| frame-slot operands, disp8 | 19.1% |
| frame-slot operands, disp32 | 9.9% |
| branches | 13.7% (rel32 most of it; now mostly rel8) |
| `movabs` | 7.2% (tag masks `0xffffffffffff` ×174 in the hot set, 157 followed by `and`) |
| nop / padding | 3.2% |

11.6% of hot-line bytes lie in blocks with no samples: cold code sharing a
line with hot code.

- **Frame-slot traffic (29%)** is S5's problem (tagged values in
  callee-saved registers across GC points; parked on brass branch
  `s5-tagged-callee-saved`). Splittable tagged intervals would remove most
  of the reloads around calls; at 29% of hot bytes it is the largest single
  lever on hot code size.
- **Constants pooled in frame slots** (0, 1, the tag masks) cost a load and
  a disp8 operand each where an immediate would do.
- **Truthy + branch fusion** in il2mir: the classifier's merge is a value
  the following `br_if` tests again; threading the constant arms straight to
  the branch targets removes the ~470K critical-edge taken branches and the
  merge's code.

## Measuring

`BRASS_BLOCK_MAP=/path/map.txt bronze build ...` writes the block map;
together with `perf record -b` it names each taken branch by the blocks it
leaves and enters, and with IBS each hot line by its blocks.

## Parked: splittable tagged intervals (chunk 12)

gcref and tagged intervals were made splittable like any value, with each GC
point's stack map listing only the live references in their slot at that point
(callee-saved register residents covered by the conservative pinning scan), and
S5's rule re-applied on top. Correct under every GC stress mode and the pin
matrix, x64 and aarch64. It did not pay: three_meshes cycles +1.0%,
instructions +2.2%, setProgram +5.3% bytes, cpu -0.4% (noise). Splitting adds
copies and edge moves that cancel the slot traffic it removes, so frame-slot
operands are not the lever they looked like. Parked on brass branch
`c12-splittable-tagged`. Untried and cheap: order frame slots hottest-first so
most slot operands take 8-bit displacements (15.4K of 20.5K hot slot operands
use 32-bit ones).

## Next: inlining and call overhead

Chunk 13, the call edges, what a call costs in cycles, and guarded inlining
reaching the frustum chain, measured and parked: [aot-inlining.md](aot-inlining.md).
