# The program of the Kokiri-Hyrule Field loss, read statically

## Summary

- **The stages run on the CPU now** (new: `tools/meshlab/irpy.py`, `cpu_draw.py`). The task stage's IR, run lane by lane
  over the sane capture (`work/runs/hang_hf_cap/meshcap/frame.bin`, present 3665, draw #230), keeps **exactly the 262
  meshlets the GPU kept** (CONFIRMED: the same set as the capture's map). The mesh stage's IR on those 262 groups gives
  13,344 triangles, all finite, clip coordinates under 12.1 (section 4). So the reading below is checked against data, and any
  input can be changed and the draw run again without a device.
- **The mesh stage is the lit meshlet stage plus per-vertex light probes.** Its first 16 output components (clip
  position, view normal, world position, handedness, view tangent, texture coordinates), its indices and its primitive
  count are **bit-equal to `e67ab18860ce163e`'s** (the stage `meshlet_lit.mesh` replaces) on the capture's groups
  (CONFIRMED, CPU: both IRs on the same meshlets, section 4). The other 12 components are spherical-harmonic light from
  a probe grid read through five guest pointers. M6e's guess "skinned" is wrong: nothing is skinned.
- **590 groups is not a sign of garbage, and not a heavy frame.** The draw's shape has 614 meshlets in 20 task workgroups (CONFIRMED: the
  model's count word, the CPU's `(614 + 31) >> 5`). 262 are kept in the capture because the camera stands inside the
  object and the frustum test culls 349. With the camera **7.4 units further back** the same data keeps 585, at 10
  units 613 (CONFIRMED, CPU replay); those 585 groups are 29,880 small triangles that cover 0.17 of the target in all,
  as the capture's 262 do (0.18). Garbage does not give 590: a garbage model buffer gives 0 or 640, a NaN matrix or a
  zeroed view buffer 614, and all of those emit nothing the rasterizer draws (section 2).
- **No stale input was found that makes this draw expensive to rasterize** while the task stage still keeps hundreds of
  groups (section 2): positions far enough to matter are culled by the task stage's own far test, NaN is dropped by the
  rasterizer, zeros collapse to nothing. The draw's depth test is EQUAL with depth writes off (CONFIRMED: the capture's
  dynamic state), behind a depth prepass of the same meshlets, so its fragment stage runs about once a pixel whatever the
  triangle sizes. The fragment stage has no discard and one bounded loop (section 3).
- **What is left** (INFERRED): the load itself. This is the one stage of the inventory whose compile needs both RADV's
  mesh scratch ring and VGPR spills (M6e), and the losing frame gives it 2.25 times its usual workgroups in one draw. No
  harness run ever drew more than 42 groups of it. Section 5 gives a one-run experiment that separates "many groups of
  this stage" from "the map change", and what to read in a capture of the losing frame.

## 1. The stages

Names: `c0` the driver bank (the stage's storage-buffer table: task from `c0+0x670`, mesh from `c0+0x770`, 16 bytes a
slot, a guest address each), `c3` the view's buffer, `c4` the object's, `c5` the material's, `c6` the lighting's.
Offsets are bytes. "By address" = a load at a guest address through the memory windows (no bound but the windows'
clamp); a storage-slot load is the same in this back end (the slot's guest address plus an offset, not checked against
the buffer's size).

### 1.1 The task stage `2c56dc1006942f74` (32 lanes, one meshlet a lane)

```glsl
// model: the model's buffer, task slot 0 (c0+0x670). A file of the game's (guest 0x7f.. : the model file's pool).
//   +0 origin (3 floats)  +12 scale  +16 format flags  +20.. 16 cumulative meshlet counts (one a level of detail)
//   +84 meshlet records, record_bytes = 8 * (4 + bitCount(flags & 0xc00) + bitCount(flags & 0x1f000))
uint  shift = (c3.u[1056] * 4 + ((c3.u[1064] & 1) != 0 ? 16 : 0)) & 28;      // the view's number: which nibble
uint  lod   = (c4.u[92] >> shift) & 15;                                       // the object's level for this view
uint  first = lod != 0 ? model.u[16 + 4 * lod] : 0;
uint  count = lod != 0 ? model.u[20 + 4 * lod] - first : model.u[20];
uint  id    = task_workgroup * 32 + lane;                                     // IsbeRead(0) * 32 + lane
bool  keep  = false;
if (id < count) {
    // by address (c0+0x670's pointer + offset): the record's words 0-4 and, for the cone, word 6
    Record r = model + 84 + (first + id) * record_bytes;
    vec3 lo = vec3(r.min21) * model.scale + model.origin;       // words 0-1: three 21-bit fields
    vec3 hi = lo + vec3(r.size21) * model.scale;                // words 2-3: three 21-bit fields
    mat3x4 mv = c3.cull_view /* 2576..2620 */ * (c4.model /* 0..44 */, + c4.offset /* 64..72 */);
    vec3 centre = mv * ((lo + hi) / 2);  float radius = length(mv * hi - centre);
    uint flags = c3.u[1920];  uint result = 1;
    if (flags & 2)  cone(r.axis /* word 4 bytes 0-1, octahedral */, r.cutoff /* word 6 bits 10-15 */, c3.f[2728]);
    if (flags & 4)  sphere against near c3.f[2512] and far c3.f[2516] (-1e7 / +1e7 instead under flags & 1 / & 64)
                    and the four planes c3.f[2928..2988];
    if (flags & 8)  the box's 8 corners against the same four planes;
    if (flags & 16) the depth grid: the box's screen rectangle in cells, both ends clamped to [0, W-1] x [0, H-1],
                    W = c3.u[3088], H = c3.u[3092], cells = floats at c3 + 3120 + 4 * (row * W + column);
                    culled if the nearest corner is behind the rectangle's farthest cell;
    if (flags & 32) the same with the corners through the clip matrix c3.f[2672..2732];
    keep = (result & 1) != 0 || (c3.u[2320] & 4) != 0;          // a debug switch: keep every meshlet
}
payload.word[4 + rank of this lane among the kept] = first + id;
payload.byte[4] = bitCount(kept lanes);                          // the mesh groups this task workgroup emits, 0..32
```

| Read | Address from | Bound | Feeds |
|---|---|---|---|
| `c3.u[1056]`, `c3.u[1064]`, `c4.u[92]` | uniform | 4 bits | which count range (level of detail) |
| `model.u[16 + 4 lod]`, `[20 + 4 lod]`, `[20]` | slot 0 | lod < 16 | `first`, `count`: **how many lanes can be kept** |
| `model` +0..+16 | slot 0 | fixed | box positions, record size |
| record words 0-4, 6 | by address: `c0+0x670` + 84 + (first + id) * record_bytes | id < count and id < 32 * tasks; `first` and `record_bytes` unbounded (memory) | every cull test |
| `c3` 2576..2620, `c4` 0..44, 64..72 | uniform | fixed | view-space box (all tests) |
| `c3.u[1920]` | uniform | 6 bits | **which tests run**; 0 = none, everything kept |
| `c3.u[2320] & 4` | uniform | 1 bit | keep everything |
| `c3` 2512, 2516, 2928..2988, 2672..2732 | uniform | fixed | frustum tests, projection for the grid |
| `c3.u[3088]`, `[3092]`, cells from +3120 | uniform, indexed | the rectangle is clamped to W, H, **W and H themselves are not bounded** (M0's clamp is not in `gpu/direct`: INFERRED, no match for it there) | occlusion; the stage's only loops |

The group count a task workgroup emits is the number of kept lanes. Over the draw it is at most
`min(32 * tasks, count)`; `tasks` comes from the CPU (section 2).

Which bit runs which test is INFERRED from where the IR tests the bit and CONFIRMED in effect by switching bits in the
replay (capture: flags `0x14`): `0` keeps 614, `0x4` 265, `0x10` 610, `0x14` 262, `0x16` 262, `0x1c` 255, `0x34` 254,
`0x3f` 247.

### 1.2 The mesh stage `40d0c0768e874600` (32 lanes, 126 vertices and primitives declared, 28 output components)

Storage slots (mesh table): slot 0 `c0+0x770` a 192-byte lighting block, slot 1 `c0+0x780` **a table of five guest
pointers** (64 bytes), slot 2 `c0+0x790` the model's buffer (the task stage's slot 0). `c0+0x880` is listed and never
read (M6e, oddity 9).

```glsl
uint id = payload.word[4 + mesh_group_index];                    // IsbeRead(16 + 4 * IsbeRead(0)): the meshlet
Meshlet s = read_meshlet(model /* slot 2 */, id);                // meshlet.glsl: record words 0, 1, 4-9
// two vertices and four triangles a lane, as in every 2c56 mesh stage (M6e, oddities 1-3)
for (k in 0..1) { v = min(lane + 32 * k, s.vertices - 1);
    vec3 p     = read_position(model, s, v);                     // stream bits * model.scale + model.origin
    vec3 world = c4.model /* 0..44 */ * p;
    gl_Position = vulkan_clip(c3.proj /* 112..172 */ * (c3.view /* 0..44 */ * (world + c4.offset /* 64..72 */)));
    out world position, view normal, view tangent, handedness, uv (c5 0..20): exactly meshlet_lit.mesh's
    out sh[12] = probe_light(world + c4.offset);                 // below
}
for (k in 0..3) { p = min(lane + 32 * k, s.primitives - 1);
    indices[p] = three fields of index_bits = findMSB(s.vertices - 1) + 1 bits from the index stream, & 255 }
if (lane == 0) primitive_count = s.primitives;                   // record word 4, bits 16-23; the epilogue cuts it to 126
// (vertex count: always 126, the translator's epilogue)

vec4[3] probe_light(vec3 w) {                                    // every load below is by address
    Grid g = *ptr[1];                 // +16 origin (3 floats), +28 cell size, +32 cells in x, y, z
    uint stride0 = ptr[0]->u[0] >> 4, stride1 = ptr[0]->u[1] >> 4, stride2 = ptr[0]->u[2] >> 4;
    vec3 q = (w - g.origin) / g.cell;  ivec3 c = clamp(ivec3(q), 0, g.cells - 1);
    Cell a = ptr[2][((c.z * g.cells.y + c.y) * g.cells.x + c.x) * stride0];      // 16 bytes: first, n, base
    vec3 f = saturate(q - c) * a.n;    uvec3 d = min(uvec3(f), a.n - 1);
    uint leaf = a.first + (d.z * a.n + d.y) * a.n + d.x;
    Leaf l = ptr[3][leaf * stride1], l2 = ptr[3][leaf * stride1 + 1];            // 8 probe numbers of 16 bits; a scale and offset
    vec3 t = saturate(f - d) * l2.scale + l2.offset;                             // trilinear weights
    for each of the 8 corners: rows 1..7 of ptr[4][(a.base + l.probe[i]) * stride2]    // 7 x 16 bytes a probe
    return the weighted sum, mixed with slot 0's block (+48..+111) by c6.f[900], scaled by c6.f[544], c6.f[548], c6.f[80..88];
}
```

| Read | Address from | Bound | Feeds |
|---|---|---|---|
| payload word | the task header's payload (the task stage's write) | group index < 32 | the meshlet id: **everything** |
| `model` +0..+16 (origin, scale, flags) | slot 2 | fixed | **gl_Position**, record size |
| record words 0, 1, 4, 5, 6 (7-9 for the attributes) | slot 2 + 84 + id * record_bytes | none (id is the task's, checked there against `count`) | **gl_Position** (origin fields, widths, the stream's start), **the primitive count** (word 4), index width |
| vertex stream: 3 words a vertex | slot 2 + 84 + bits / 8, bits = 8 * (word5 >> 4) + v * (word6 & 1023) | **none**: word 5 gives up to 256 MB | **gl_Position**, normal, tangent, uv |
| index stream: 2 words a triangle | after the vertex stream | none | **the indices**, each & 255 and cut to 125 by the epilogue |
| `c4` 0..44, 64..72; `c3` 0..44, 112..172 | uniform | fixed | **gl_Position** |
| `c5` 0..20 | uniform | fixed | uv |
| `ptr[0..4]` | slot 1, 5 quadwords | fixed | the probe loads' addresses |
| grid header, cell, leaf, probes | by address: `ptr[1]`, `ptr[2] + i * stride0 * 16`, `ptr[3] + ...`, `ptr[4] + ...` | the cell is clamped to the grid's own counts, the leaf to the cell's own `n`; **counts, strides, `first`, `base` and the probe numbers come from memory: no bound** | only the 12 light components. **Not** the position, the indices or the counts |
| slot 0 +48..+111, `c6` 80..88, 544, 548, 900 | storage / uniform | fixed | the 12 light components |

There is no cull or visibility decision in the mesh stage and no loop (CONFIRMED: the IR has none; M6e). 139 loads by
address a lane pair: 120 of 16 bytes for the probes, 19 words.

## 2. Stale or garbage inputs: what each would do

All CONFIRMED on the CPU (`cpu_draw.py` on draw #230 with the input overwritten, 40 groups of the mesh stage where it
ran) unless marked. "Drawn" = triangles with finite corners that have area inside the clip volume.

| Input changed | Task: kept of 640 lanes | Mesh: what the rasterizer gets |
|---|---|---|
| nothing (the capture) | 262 | all 262 groups: 13,344 triangles, all finite, 11,389 with area in the clip volume, 0.18 targets summed, largest clip coordinate 12 |
| camera 7.4 units back (`c3` 44 and 2620 from -133.06 to -140.5), grid open | 585 | all 585 groups: 29,880 triangles, all finite, 28,521 with area, **0.17 targets summed**, largest clip coordinate 17 |
| `c3.u[1920] = 0` (no test) or `c3.u[2320] = 4` | 614 | the whole object, as above |
| `c3` all zero (a buffer not written yet) | 614 | every position (0, 0, 0, 0): nothing drawn |
| `c4` matrix NaN, or `c4` offset NaN, or model scale NaN, or `c3` cull view NaN | 614 | every corner NaN: nothing drawn |
| `c4` matrix zero | 0 | - |
| `c4` translation or offset 1e7, matrix x 1e5, model scale 2000 | 0 or 1 (the far test, 25,000) | (1 group: coordinates 5.9e7, outside) |
| planes `c3` 2928..2988 zero | 468 | INFERRED: as the capture, more of it |
| grid cells all 0, or W = 0, or near = far = 0 | 1 | - |
| grid cells all 1e30 (nothing occludes) | 265 | - |
| `c4.u[92]` nibble 1, 2 (another level) | 226, 210 | another level's meshlets: valid |
| model buffer zero | 0 (count 0) | - |
| model buffer random bytes (5 seeds) | 0, 0, 640, 0, 640 | seed 3: 3,708 triangles, none with area (the vertices collapse) |
| model buffer moved by 40 bytes, 4 KB, 64 KB (by 4 bytes: 0 kept) | 640 | 40 bytes, 4 KB: 3,422 and 3,226 triangles, none with area |
| model buffer = another model's (blobs 1583, 1588) | 83, 88 (its count) | 3,108 valid triangles, 0.03 targets |
| model buffer = other data (blob 6: a 10 MB table; 26, 390, 1587) | 640; 1; 0; 0 | blob 6: primitive counts 0, nothing |
| model count word `+20` = 0xffffffff | 288 | lanes past 614 read records past the list: a few kept |

Read against the three questions:

**(a) Many more groups.** The count is `min(32 * tasks, count)` minus what the tests cull. `tasks` is the CPU's:
`render_job_record` (`f_80084de0`, the item loop's draw at `0x80089fa8`, CONFIRMED by decompiling it) calls
`DrawMeshTasks(0, (n + 31) >> 5)` with `n` = the shape's word `+0x6a4` for a shape of kind 2 (`shape + 0x69f`), the shape
being the item's drawable `+0x68`. Here n = 614 = the model buffer's `+20` (CONFIRMED: 20 tasks, count 614). The return
address 0x8008533c the capture records is the list's (`BeginRecording` in the same function), not the draw's. So a **590
groups from 20 workgroups frame is this same shape (609..640 lanes possible, 614 by the capture) with about 24 meshlets
culled instead of 352.** Inputs that do that: the camera seeing the whole object (7 to 10 units further back is enough, 585 and 613 kept:
the object is a room of 15.6 x 9.7 x 12.9 units and the capture's camera is inside it), or `c3.u[1920]` without bit 2. Inputs that
give *exactly* 614: flags 0, the keep-all bit, a zeroed `c3`, NaN anywhere in the transforms. Exactly 640: a model
buffer whose origin or scale is NaN or whose count word is larger than 640 (reused memory). **A count between, like
590, needs working tests on valid data.** INFERRED: the losing frame is an ordinary frame that shows the whole object
(a camera cut at the map change), and the exact count will tell: 614 or 640 would mean broken inputs, 590 does not.

**(b) Non-finite or huge positions.** `gl_Position` depends only on: the payload word, the model buffer (origin,
scale, record words 0, 1, 5, 6, the stream), `c4` 0..44 and 64..72, `c3` 0..44 and 112..172. NaN in any of them gives
NaN corners (dropped by the rasterizer; the task stage then keeps all 614, since every test is an ordered compare).
Huge finite values are culled by the task stage's far test before the mesh stage runs, **as long as both stages see
the same memory**: the task stage runs earlier (the job's task step), so huge positions with hundreds of groups need
the buffers to change between the two (INFERRED possible, not seen).

**(c) Huge triangles covering the target many times.** Not found with any single input (table). And this draw is
protected twice: the task stage's far and frustum tests, and the depth state. The capture's dynamic state for #230 is
depth test on, **compare EQUAL, write off**, cull back, one colour target (format 122), no blending, one sample; draw #271 (`6993fab261c7e4e9`,
mesh `ef1f1c50d829bf4e`, the depth-only stage, 20 tasks, the same 262 groups, not gated) lays the depth before it. Both
stages compute the position with the same arithmetic (CONFIRMED on the CPU: `40d0`'s position, indices and count bit-equal to `ef1f`'s on 24 groups, to `e67a`'s on all 262), so the
fragment stage runs once a covered pixel, and where the two passes saw different data nearly never. The one hole
(INFERRED): the projection has no far plane (`c3` 112..172: rows `[0 0 -1 -0.2]`, `[0 0 -1 0]`), so anything farther
than about 3 million units has depth exactly 1.0 after the conversion, equal to a cleared depth buffer: such triangles
would pass EQUAL on every sky pixel, each of them. That needs the huge positions of (b).

## 3. The fragment stage `0801cca176fd6c81` in outline

From the IR and the census row (M6h's table; INFERRED unless marked):

- 1,549 native instructions, 2,979 IR; RADV: 2,344 instructions, 112 VGPRs, no scratch, 9 waves a SIMD; ACO's estimate
  4,179 cycles a wave = **131 SIMD cycles a pixel** with the loop weighted 8 trips.
- Inputs: 8 lines, 24 components: position, view normal, world position, handedness, tangent, uv, and the mesh
  stage's 12 light components. One colour target (4 components, NaN-guarded), **no discard, no depth output**, so early
  depth applies.
- **11 texture samples**: 8 of 2D textures with implicit LOD (handles at `c2` 32, 40, 48 by the material's uv; 128,
  144, 200 by screen position; 168, 176), 2 of a cube with explicit LOD (`c2` 184: the reflection), 1 of a 3D texture
  in a branch (`c2` 136). None in the loop.
- **One loop**, the light list: `trips = min(word of storage slot 0, c6.u[1808]) + 1` (signed min; `c6.u[1808]` is the
  "c6[452]" of the earlier test). Trip 0 is the directional light from `c6` 368..376; each later trip reads a list
  word (slot 0, index from `c3.u[2384]`) and a light record of 96 bytes (slot 1: one word and four 16-byte rows; type 5
  takes a branch of its own). The loop body is 920 of the IR's 1,662 lines, with 27 square roots, powers and
  logarithms a trip: roughly 60 % of the stage a trip. 10 storage loads in all, none by a free pointer.
- Cost of covering 1536 x 864 once: 1.33 M pixels x 131 cycles = 1.7e8 SIMD cycles, **0.5 ms** on 160 SIMDs at
  2.3 GHz (64 trips: about 3 ms). **Seconds need hundreds to thousands of full-target layers**, which EQUAL depth does
  not allow (section 2c). Nothing in the stage can run long on its own with the loop capped.

## 4. The sane capture against the reading

`work/runs/hang_hf_cap/meshcap/frame.bin`, present 3665, draw #230 (`work/hfdecomp/check.py`, `cpu_draw.py`):

| Field | Value | As read? |
|---|---|---|
| draw | first 0, **20 tasks**, list kind 2, gated, 1536x864, header: 262 groups | - |
| task slot 0 = mesh slot 2 | guest 0x7f67944000, 3.05 MB | yes: one model buffer |
| model +0..+16 | origin (-15.76, -1.05, -6.83), scale 0.0005, flags 0x1061 (record 40 bytes) | yes |
| model +20.. | 614, 1151, 1644, 2099, 2507, 2854, 3137, 3352, 3489, 3538 x 7: cumulative, 10 levels | yes: 614 -> 20 tasks |
| `c4.u[92]`, `c3.u[1056]`, `c3.u[1064]` | 0, 0, 0xe52 | level 0: range [0, 614) |
| `c3.u[1920]`, `c3.u[2320]` | 0x14, 0 | frustum + grid |
| `c3.u[3088]`, `[3092]` | 32, 18 | the grid M0 found |
| `c3` 2512, 2516 | 0.1, 25000 | near, far |
| `c3` 2576..2620 = `c3` 0..44 | the same view matrix | cull view = draw view |
| `c3` 112..172 | (1.206, 2.144, [0 0 -1 -0.2], [0 0 -1 0]) | a projection without far plane |
| `c4` 0..44, 64..72 | identity rotation, translation (-114.0, 114.75, -38.4); offset 0 | yes |
| meshlets of the range | 9..64 vertices, 3..65 triangles, none over 64 / 126, no stream past the buffer; 31,434 triangles in all | yes; oddity 2 does not bite |
| mesh slot 1 | 0x108c6e8000, +0x40, +0x80, +0x680, 0x108e878680 (all captured), then zeros | five pointers |
| grid header (`ptr[1]`) | cell 64.0, cells 6 x 2 x 4; strides (`ptr[0]`) 1, 2, 8 | yes |
| **task stage on the CPU** | 262 kept, per task 32 32 32 29 14 0 0 0 0 1 2 0 5 15 17 31 29 10 11 2 | **the capture's map exactly** |
| **mesh stage on the CPU**, all 262 groups | 13,344 triangles (= the kept records' counts summed), none non-finite, 11,389 with area in the clip volume, their areas 0.18 targets in all, largest clip coordinate 12.1 | finite, on the target |
| `40d0` against `e67a` (slot 0 given the model), all 262 groups | 262 of 262 equal in the 16 components, the indices and the count | bit-equal |
| `40d0` against the depth-only `ef1f1c50d829bf4e` (the prepass's stage), 24 groups | 24 of 24 equal in the position's 4 components, the indices and the count | bit-equal: EQUAL depth holds |

One thing off: task 11's payload count word holds 0x101 in the capture although the task kept none and has no map
entry (the CPU replay writes 0). INFERRED: a leftover of the payload area (a task that emits nothing need not be read).
Not followed.

The camera is at (-124.3, 116.1, -38.4), inside the object: the 614 meshlets' boxes span (-129.8, 113.7, -45.2) to
(-114.2, 123.4, -32.3) in the world, a room of 15.6 x 9.7 x 12.9 units. That is why the frustum test culls 349 of 614
here and why a small camera move changes the count so much.

## 5. What to do next

**In a capture of the losing frame** (`HOS_DBG=meshdry,meshcap:<present>`; `meshdry` keeps the device), for this
program's draw. `cpu_draw.py` prints the first six lines' worth by itself:

1. `tasks` (McDraw.y) and the header's group count; the model buffer's `+20` and the nibble of `c4.u[92]`: is it the
   614 shape (guest 0x7f67944000)? Count exactly 614 or 640, or between?
2. **The CPU replay against the GPU's map** (`cpu_draw.py`, "the same meshlets"). False means the memory the task
   stage read on the GPU was not what the frame's end holds: stale data caught in the act (the capture reads the game's
   buffers at the frame's end). This is the direct test of the stale-buffer idea.
3. `c3.u[1920]` (0x14 expected), `c3.u[2320]` (0), `c3` 2512 / 2516 (0.1 / 25000), `c3` 0..44 against 2576..2620
   (equal), `c3` 112..172 (finite), `c4` 0..44 and 64..72 (finite, a translation near the camera).
4. `cpu_draw.py`'s mesh line: `not_finite`, `largest_coordinate`, `at_depth_1`, the coverage. A sane frame of this
   shape has coordinates under 20 and a coverage of about 0.2 targets (262 and 585 groups alike).
5. The dynamic state: depth test 1, write 0, compare 2 (EQUAL); and that the depth-only draw of the same model buffer
   (`ef1f1c50d829bf4e`) precedes it with the same group count.
6. Mesh slot 1's five pointers and the grid header (cell size, counts): only the lighting depends on them, but a freed
   probe grid would show here first (pointers into memory the capture could not read: "loads of memory not captured").

**One run that separates load from map change** (for the agent that owns the GPU): the same shape with its culling off
in a scene that stands. `cap_poke.py <frame.bin> <out.bin> 230 6.c3.1920=0` writes a capture whose task stage keeps all
614 meshlets (CONFIRMED on the CPU; `work/hfdecomp/frame_nocull.bin` is made); `meshlab run` with `--tasks` on it draws
#230 with 614 groups, 14 times any `cfebb562` draw the harness has drawn. In the game the switch exists:
**`HOS_DBG=tasknocull`** (`direct_shader.cpp`, M4c) takes the game's keep-all bit (`c3.u[2320] & 4`) as set in every task
stage, so standing in this room the draw emits 614 groups every frame, more than the losing frame's 590. A loss there, without any map change, puts the
cause in this stage's compile at this load (the scratch ring, the spills) and not in the data; a run that survives
minutes of it puts the load aside and leaves the map change's own events (buffers freed and bound again, the probe grid
replaced, the render condition).

**A game patch, if a capture shows stale data.** The draw is issued in `render_job_record` (`f_80084de0`) at
`0x80089fa8`: shape kind 2, `DrawMeshTasks(0, (shape[+0x6a4] + 31) >> 5)` through the pointer at 0x84cae7a0. A hook
there has the item, its drawable and the shape in hand and can skip the draw; the condition a capture would justify is
"the bound model buffer's `+20` (level 0 count) differs from `shape[+0x6a4]`" (one guest read; the buffer was replaced)
or "the drawable's object block is not finite". Nothing here says such a gate is needed: no stale input was found that
hurts (section 2).

**A hand rewrite: yes, and it is the cheapest experiment on the compile.** The stage is `meshlet_lit.mesh` with
`MODEL_SLOT=2` plus the probe lookup (section 1.2: about 60 lines of GLSL over `common.glsl`'s `Words`), and
`cpu_draw.py --against` checks the first 16 components without a device; the 12 light components need the lookup
written and compared (`meshlab compare`, or irpy on a second IR). The rewrites have no stage buffer in workgroup
memory, which is what pushes this stage to 30,378 bytes of LDS, into the scratch ring and into spills (M6e, oddities
13-14): the lit rewrite needs 12,490 bytes against 22,218. No rewrite is committed here: the probe part is read but
not written out, and nothing may be enabled before it compares equal.

## 6. Tools and tests

| What | Where |
|---|---|
| An IR print run on the CPU: the structured flat print turned into a Python function, one lane at a time (loops, phis by predecessor block, the carry and zero pseudo-operations, by-address and storage loads through callbacks) | `tools/meshlab/irpy.py` |
| A capture read in Python (programs, blobs, draws with their buffers, map and payload; guest memory by address) | `tools/meshlab/capture.py` |
| A captured task+mesh draw on the CPU, with inputs overwritten; a second mesh stage compared | `tools/meshlab/cpu_draw.py` |
| A capture with words of a draw's uniform buffers changed | `tools/meshlab/cap_poke.py` |
| `meshtool --task <task.bin> <prefix> <mesh.bin>`, and `<prefix>.pre.c` for mesh stages: the flat print taken **before** the SPIR-V is made (emitting consumes the carry and zero pseudo-operations; the later prints show them as `Void()`, which cannot be run). `build.sh` takes `MESHLAB_OUT` | `tools/meshlab/meshtool.cpp`, `build.sh` |

```
MESHLAB_OUT=work/hfdecomp/bin tools/meshlab/build.sh
T=work/proto/direct-m0/tm
work/hfdecomp/bin/meshtool --task $T/stages/6_2c56dc1006942f74.bin work/hfdecomp/ir/task $T/stages/7_40d0c0768e874600.bin
work/hfdecomp/bin/meshtool $T/stages/7_40d0c0768e874600.bin work/hfdecomp/ir/mesh $T/stages/6_2c56dc1006942f74.bin
work/venv/bin/python -I tools/meshlab/cpu_draw.py work/runs/hang_hf_cap/meshcap/frame.bin 230 \
    --task-ir work/hfdecomp/ir/task.pre.c --mesh-ir work/hfdecomp/ir/mesh.pre.c [--groups 40] [model=random:3 ...]
```

The task replay takes 0.6 s; the mesh stage 2.3 s a group (7,500 IR values, 32 lanes). Limits: one lane at a time (a
ballot is this lane alone, which is what these stages use it for: uniform loads, and the task stage's final count);
`fma` is exact, the other operations are Python's doubles rounded to 32 bits; texture operations are not there
(fragment stages do not run).

Tests: `HOOKSHOT_VULKAN_HEADERS=/usr/include work/venv/bin/python -I -B tests/run_all.py`: 296 passed, 5 failed (the
baseline five), 19 skipped. No game code, capture or IR print is committed. Local: `work/hfdecomp/` (`ir/` the prints,
`runs2/` the replays' outputs, `ghidra/f_80084de0.c`, `check.py`, `frame_nocull.bin`).
