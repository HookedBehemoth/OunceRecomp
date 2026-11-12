# M0: task and mesh shaders (2026-11-12)

Analysis only; nothing was run. Tools (all under `work/proto/direct-m0/tm/`, nothing leaves the PC): `seed.py` takes
every task (NVN stage 6), mesh (7) and compute (5) stage out of the romfs seed (`hookshot_shaders.bin`, code key as
`vk_shader.cpp` computes it); `tmtool` (`build.sh`, links the `vk_eden_*` / `vk_sirit_*` objects of `work/sa/build`)
lowers and translates one stage as `translate_mesh_into` does and writes the IR, a structured print of it (`*.ir.c`) and
SPIR-V; `progmap.py` maps the program keys of a run log (`shader objects: fallback ... (task + mesh)`) back to their task
and mesh code keys. Output in `tm/out/`. The game has **20 distinct task stages** (one, `64ba846654e63ea4`, does not
lower: "native stage buffer access mode", as in play).

**Scenes** (CONFIRMED, `e7_gold_hf` / `e7_gold_kokiri`, all 40 / 22 program keys mapped): Hyrule Field draws task
`2c56dc1006942f74` (33 programs), `8fb3abacd57f1ee6` (2), `1813f9ceef04a63f` (2), `d91e4f46becaba90` (2),
`919b5a760b54adf1` (1); Kokiri `2c56` (15), `563d4c2d98c4a4da`, `8fb3`, `1813` (2 each), `919b` (1).

## 1. What the task stages do

Every task stage reads `IsbeRead(0)` = its task workgroup index, which the translator turns into workgroup + the draw's
first task (`EmitIsbeRead`). None reads gl_DrawID, instance or vertex builtins. The mesh group count is the payload's
byte 4 (`IsbeWriteAttribute(4, n)`), the translator's `TaskEpilogue` emits `min(n, task threads)`. CONFIRMED (IR):

| Family (code keys) | Input | Payload written | Mesh groups | Data-bounded loops |
|---|---|---|---|---|
| **A** meshlet cull: `2c56` (+ `340f`, `6e08`, `7e9d`, `ab1c`, `b588`, `fdad`, 256 B) | lane = meshlet `wg*32+lane` of a range in ssbo c0+0x670 (+16/+20); meshlet records by address (5 words at +84..+100); c3 view constants; the coarse depth grid in **c3 words from +3120**, its width / height **c3+3088 / +3092** | ids of the kept meshlets, compacted by ballot, at bytes 16 + 4k | popcount of kept lanes, 0..32 | the depth-grid walk (two copies): rows x columns of the meshlet's screen box, both ends clamped to `[0, W-1]`, `[0, H-1]`, **W, H read from c3** |
| **B** instance/patch: `8fb3` (+ `c8a0`, `b50a`, `577e`, 3200 B) | wg = an instance (wg > 4095: nothing); its list entry ssbo c0+0x680 at `wg*4 + 64` (past the culling list's 64-byte header, section 3); item range `n = min(end - start, 256)` from a table by address | +16 instance, +20 kept count K, three arrays of 256 words at +24, +1048, +2072 | `ceil(2K/32)`, 0..16 | **`for (i = 0; ; i++) { ...; if (i + 1 > (n - 1) >> 5) break; }`** |
| **B'** `919b` (+ `a5b4`), `1813` (+ `0c4a`) | as B (`1813` also walks the grid, its bounds hard-coded 31 x 17) | as B | `ceil(K/8)`, 0..32 | as B; `1813`'s grid walk is constant-bounded |
| **C** `d91e` (+ `e85f`, 1152 B) | as B | +16, +20, one array at +24 | `ceil(K/32)`, 0..8 | as B but guarded: `n == 0` writes 0 and skips; also exits at 8 |
| `563d` (Kokiri) | none | `wg*32+lane` per lane at 16 + 4k | 32 | none |
| `a1cb` (1 thread) | c3+16 | +16 = c3+16 | `ceil(c3[16]/32)`, unbounded; the epilogue's cap of 1 (its thread count) cuts it to 1 (INFERRED: wrong if c3+16 > 32; not in these scenes) | none |

**The B / B' runaway** (CONFIRMED in the native code, not a translation artefact: `8fb3` native +21 `SHF.R.U32.HI` of
`n - 1` by 5, +22 `ISETP.GE.U32 ..., RZ`, always true; `919b`, `1813`, `c8a0`, `b50a`, `577e`, `a5b4`, `0c4a` the
same, only family C tests `n != 0`): with **n = 0 the loop runs 2^27 times** per task workgroup (each iteration a
ballot, shuffles and a skipped body). On the console n is never 0, because every listed instance has items. It is 0
whenever the instance list or the item table is read as zeros: read before the culling dispatch wrote it, a fresh
or cleared allocation, or the zeroed slot of section 3. INFERRED: this is the best candidate for **hang 1** (task
shaders on the ACE ring under faster timing, `nocap_allcull` still hung with the grid walk removed). `8fb3`, `919b` and
`1813` programs are named in the `nocap_allcull` and `vc0` logs before the loss; the other lost runs name too few keys to
tell.

**The A walk** (CONFIRMED): at most W x H cells a lane, a dense 4-byte cell at `c3 + 3120 + 4*(row*W + col)`.
`1813` hard-codes the same grid as 32 x 18 (= readback2.md's grid made by `3018813b775edcb6`), so W, H = 32, 18 in
play (INFERRED). W and H come from a uniform buffer: stale or garbage words give an unbounded walk (**hang 2**). Read
as zeros it is one iteration and harmless. `RX_VK_MESH_NOCULL` removes the walk together with the culling.

**Side effects** (CONFIRMED): the HF / Kokiri task stages only read memory and write the payload. `340f`, `7e9d` and
`ab1c` add 8 statistics counters (ssbo c0+0x670, +288) under `c3+2320 & 8`, a debug flag. Two mesh stages (`6ba0b411`
of `8fb3`, `9d026703` of `d91e`) add their primitive counts to ssbo c0+0x780 (+32, +44). The split below keeps the mesh
stage as it is, so those counters stay correct.

A loop in `340f` / `b588` exits only when the counter reaches `0xffffffff` (native +108 `ISETP.NE ... 0xffffffff`). It
is a 64-bit bit-mask scan, and how it really exits was not settled (needs the native CFG). It is not in HF or Kokiri.

## 2. Is the split mechanical?

**Not as the plan words it** (CONFIRMED). The count and payload belong to each *task workgroup*, not to each draw. A draw
starts N task workgroups (N = the draw's count, from the CPU or the indirect record; up to 4096 in B), and each one
emits 0..32 mesh groups with its own payload. Family A's payload splits into one word per mesh group: mesh `072e452f`
reads only `payload[16 + 4*meshgroup]`. Families B, B' and C share a header among the task's mesh groups: their meshes
read +16 and +20 and index the arrays at +24 / +1048 / +2072 by group and lane. So a flattened `{count, payload}` per
draw does not exist.

What is mechanical is the existing fallback's **task half alone** (`TaskEpilogue`'s `mesh_fallback` branch, already in
the translator). The task runs as a compute dispatch, one workgroup per task workgroup. Each emitted group takes a slot
from an atomic counter in the draw's header (`MESH_FALLBACK_DISPATCH_WORD`) and writes the slot's task workgroup and
local index to a map. The task copies its payload once to `payload[task]`. Then the mesh stage runs **natively** with
`vkCmdDrawMeshTasksIndirectEXT(header)`, header = `{slots, 1, 1}`, on the graphics ring only. Its `IsbeRead(0)` is the
local index and its other `IsbeRead`s go to `payload[task]` (the fallback's `FallbackSlot` / `PayloadWord` loads, with
the native mesh outputs instead of `MeshFallbackEpilogue`). Dropped against the old emulation: the mesh-as-compute
pass, the 64 MB records buffer, the generated vertex stage and two of the four barriers. What breaks it: slot order
is atomic, so primitive order within a draw changes (INFERRED harmless for the alpha-tested foliage, not for blended
draws). The payload buffer is up to N x 3200 B a draw (13 MB at N = 4096: allocate only for tasks that emit). Every
pass needs its task inputs ready before it (pre-work, fallbacks.md 3a/5).

**Folding the task into the mesh stage does not work** (CONFIRMED by the counts). One task workgroup feeds up to 32
mesh groups of up to 126 vertices and 64 primitives each, and a mesh workgroup can output at most 256 of either. The
only fold that fits runs the 32-lane task program again in every mesh group (32 times its work, the grid walk
included). Rejected.

## 3. The culling's indirect records

**Writers** (CONFIRMED, IR of the seed's compute stages). The indirect mesh draws (`kind 1, max 1, stride 0`) point into
the six double-buffered lists of `f_80362ff0` (readback2.md 1). `e6_warm_hf` logs the addresses 0x117cf04000,
0x117d03c000, 0x117d8d4000 and 0x117db24000 + 0x10 / + 0x20, which are slot bases plus a record. Their writers are
`4247f1b4c3ce6cf3`, `f0fed2bbccae7bdc`, `cb2e06ddfb9d7d27` and `3e6fe279463b020e`. The 64-byte slot header is four
16-byte records, and each writer touches a record r in only two ways:
`atomicAdd(hdr + 16r + 12, items)` (the append count, whose result is the list position) and
`atomicMax(hdr + 16r + 0, ceil(min(total, 0x1400) / 16))` (the group count). **Record = `{x = groups, w1 = 0, w2 = 0,
w3 = appended}`**. NVN reads `{count = w0, first = w1}`. w1 and w2 are written by nobody on the GPU. The CPU zeroes the
whole header each frame (`f_80362ff0`, after reading w0 and w3 of record 0 and w3 of records 1-3). INFERRED: the five
other indirect addresses in the log (0x117896a000, 0x1178982000, 0x117898e000, 0x117a4d6040, 0x117c7cc000) have
another writer, possibly `7984161db3e053b5`. A frame dump (`RX_VK_DUMP` step 000 with each dispatch's storage ranges)
in HF settles it.

**Readers.** The draws themselves; `f_80362ff0` (w0, w3); the translator, which reads the draw's first task from w1
(`EmitIsbeRead`, the `RX_VK_MESH_FIRST_WORD=1` copy). `f_801a6410` reads the CPU copies, not the header.

**Smallest change: patch the CPU, not the shaders.** In `f_80362ff0`, write `{0, 1, 1, 0}` into each of the four
records instead of zeros. The culling's atomicMax and atomicAdd then leave a valid Vulkan record `{x, 1, 1}` in place.
Nothing is copied or converted, and the draw is `vkCmdDrawMeshTasksIndirectEXT(record, 1, 16)`. Its first task is 0 by
construction (nothing ever writes w1): the translator takes `first = 0` for these draws instead of reading w1.
Replacing the four culling shaders by hash would do the same with four rewrites instead of one store site. A slot
not yet zeroed by the CPU (its first frame) holds y = z = 0 and draws nothing, which is harmless.

## 4. The smallest bounds

Two IR rewrites at translation, next to `mesh_nocull` in `vk_shader.cpp`, matched by pattern (the families share it)
and logged per code key. Both are exact for every value the console produces:

1. **B / B'**: `ShiftRightLogical32(IAdd32(UMin32(x, 256), -1), 5)`, the trip bound, becomes `UMin32(..., 7)`. n = 0
   then takes 8 empty iterations (K = 0, no mesh groups); n in 1..256 is unchanged.
2. **A**: the two clamp ends `W - 1` (`CompositeExtract(GetCbufU32x2(3, 3088), 0) - 1`) and `H - 1` (c3+3092) become
   `SMin32(W - 1, 31)` and `SMin32(H - 1, 17)`: at most 576 cells a lane, the bound `1813` has. This replaces
   `RX_VK_MESH_NOCULL` and keeps the culling.

A test can check both offline only on synthetic IR (no game code in CI). A local check is `tmtool` on the seed.

## Decision for the plan

The plan's step 3 as written (a per-draw `{count, payload}`) is wrong. Payload and count are per task workgroup, so the
"split" is the old fallback's task half plus a native mesh stage. That is a real but bounded translator change (a new
`RuntimeInfo` mode that reuses `TaskEpilogue`'s fallback branch and the slot and payload loads, with the native mesh
epilogue). Folding the task into the mesh stage is not possible. Order instead:

- **(a)** Land the two clamps of section 4 first. They are small, exact on console data, and remove the only
  data-bounded loops in the HF / Kokiri task stages. The B runaway at n = 0 is the strongest candidate yet for hang 1.
- **(b)** Then run the plan's diagnosis step 1 (native task shaders, control run) with the clamps in. If it no longer
  hangs, native task shaders are the answer and nothing is split.
- **(c)** Build the task-as-compute split only if the ACE ring still fails with the clamps in.
- **(d)** Settle the indirect records by the `f_80362ff0` header patch `{0, 1, 1, 0}` and `first = 0` for these draws.
  This needs no culling-shader replacement and no conversion copy. It still needs the writer of the five other
  indirect addresses identified.
