# A render job's target formats, from an exact source

Question: which data gives `f_80084de0` the colour/depth formats and sample count of the pass its secondary will run in
before that pass's layer list sets its targets? Answer: no data the job holds gives the formats, but the job holds an
exact pass identity, which the view record does not. The game also has its own inline path for a pass whose jobs did not
record. Together these make the format table exact: a miss sends that pass down the inline path, and no job is ever
recorded against a guess. Static analysis only. Addresses are guest VAs. Disassembly: `work/proto/engine-jobs/d_*.s`,
`tools/re/disasm.py`.

## 1. What the job has when it records

- **The view record is not an identity: it is a slot handed out in order of arrival.** CONFIRMED, `0x80084ec8..0x80084f10`
  and `0x80086db8..0x80086dec`. The recorder searches the draw queue's array of records (ctx +0x38, count +0x40) by CRC and a
  0x70-byte memcmp of the key. When no record matches, it **appends one**: `rec = views[ctx->n++]`, copies 0xc0 bytes of the
  key into it, and zeroes the slot list. This runs under ctx's mutex. The first job of any pass to reach a queue therefore
  takes that queue's next slot, so a record's address depends on which pass's jobs arrive first, which is thread timing and
  scene content.
- **That explains the alternating probe views.** CONFIRMED by data. In Kokiri the queue `0x113714c498` holds 6 keys: the 4
  depth probes (keys `0x115bd74ec8 + n*0xc0`, layer list `f_8028a2a0`) and pipeline passes 0x14 and 0x15 (1536x864, layer
  list `f_806688c0`), at records `0x113715cf80 + n*0xe0` (`engine-jobs/jp1_jobs.txt`). In `work/runs/e4_cyc2/log.txt`,
  records `..d060`, `..d140` and `..d220` switch between 384x216 depth-only and 1536x864 colour+depth from frame to frame.
  Record `..d140` ends noted at site `0x80668c84`, but in Kokiri it is a probe (`0x8028a538`). The per-frame reset of ctx
  +0x40 is INFERRED: the same addresses come back every frame. A watch on that word would confirm it.
- **The key pointer (x3) is the exact pass identity.** CONFIRMED. For 16 of the 17 keys of a Kokiri frame (all but the 128x128 pass, job vtable `0x84c297d8`), the job object (x4)
  is the scene pipeline object (`0x113b6436f8`, vtable `0x84c2b9d0`). The keys sit at fixed addresses: the pipeline's own keys
  at `job + 0x3830 + 0xd0*idx` (idx 1-3 cascades, 5-6, 0x14-0x17, 0x19-0x1a, 0x1d), and the probes' keys in their own object.
  `f_806688c0(pipeline, drawctx, idx)` reads the same storage (`0x80668910..0x80668918`: `x21 = x0 + 0x3830 + idx*0xd0`;
  `[sp+0x18] = key + 0xc0`). `idx` is an immediate at 19 of its 20 call sites: 5, 6, 0xe, 0x13-0x20; the cascade loop at
  `0x80668890` passes a variable. `f_8028a2a0(owner, drawctx, key)` takes the key pointer itself (x2, kept in x23). So the
  recorder and the job-call site name a pass by the same pointer, and that pointer does not depend on arrival order.
  INFERRED: the pointer stays stable across scenes (the pipeline object looks long-lived). Logging the key pointers over a
  scene cycle would confirm it. If the object is ever reallocated, the result is a table miss, not a wrong format.
- **What the key holds besides the formats.** CONFIRMED in engine-jobs.md section 1. It holds the target size (+0x78), the
  viewport (+0x90), the depth range and the pass flags (+0x18). No pointer to the targets and no formats were found in the
  job, key, view record, ctx, draw context or the job's 0x480 record (engine-jobs.md section 3). The sample count is 1 in
  every pass seen (e4 pass table).

## 2. Can the targets be decided before the jobs are kicked?

- **The targets are allocated inside the layer list.** CONFIRMED, callers in main.bin: the colour target's allocator
  `f_806790d0` is called from `f_80a47660` (`0x80a47700`), the scene depth (`f_8065c660`) and the probes (`f_802867f0`) from
  `f_80660320` (`0x80662890`, `0x80660a14`). Each of them calls `f_8005ed00` while the layer list records. That is after the
  kick (`f_8007fe10` / `f_80070b20`), so the images cannot exist when the jobs record without moving the pass code.
- **The formats could be computed earlier, but each pass computes them in its own way.** CONFIRMED by the descriptor stores
  before each `f_8005ed00` call:
  - scene depth: the constant agl format 0x5c (`0x8065d034`);
  - shadow array: 0x5a or 0x5c, chosen by `[pipeline+0xf0]` (`0x80669484..0x806694c8`);
  - the 1536x864 colour: the field `[obj+0x1b8]` (`0x80679410`);
  - soft-edge depth: `[cfg+0x5d8]` of a per-view array of stride 0x53d8 (`0x80666da0`).

  A hook at the kick could publish formats by reading these fields. That means a host copy of the target choice for about
  25 passes, written per pass, which breaks whenever a pass reads a field it did not read before. Not recommended.
- **A new format set comes only from a new key or from settings.** E4 noted 0 format changes in 305,574 notes for Kokiri and
  HF. Every change logged in the scene cycle is a record swapped between keys (above). INFERRED: per key, the formats change
  only with the config fields above. Keying the E4 table by key pointer over `e4_cyc` would confirm it.

## 3. The game's inline path

CONFIRMED in code. When the layer list finds **no** view record for its key, it does not skip the pass. It records the
pass's items itself, on its own thread, after its own SetRenderTargets:

- `f_806688c0` branches to `0x80668d44` and calls `f_80681320(queue, drawctx, &key, pipeline, ...)` at `0x80668d80`;
- `f_8028a2a0` branches to `0x8028a54c` and calls the same function at `0x8028a588`.

`f_80681320` builds the render info the job builds (it reads the job's +0x550 and the key's +0x18) and calls
`f_80078fc0`, the recorder's occlusion helper. INFERRED that it draws exactly what the six jobs would. A run with every
key forced down this path would confirm it by its pictures. The recorder also has a plain early return
(`0x8008693c`, taken when the key's mask misses the queue or job +0xc bit 0 is set) that creates no record.

## 4. Recommended mechanism

1. **Recorder entry** (`ENTRY_HOOK 0x80084de0`, which `host/eng_jobs.c` already has). Look up `formats[x3]`:
   - on a hit, record the secondary with those formats (`VkCommandBufferInheritanceRenderingInfo`, samples 1);
   - on a miss, take the native early return (`0x8008693c`): no view record and no slot for that key this frame.
2. **Job-call splice**, before the handles are called: `f_806688c0` at `0x80668a9c` (record found; key = `[sp+0x18]-0xc0`)
   and `f_8028a2a0` at `0x8028a3c4` (key = x23). The seam knows the layer list's current SetRenderTargets.
   - Write or refresh `formats[key]` from those targets.
   - If the jobs were recorded with formats that differ from the targets (a settings change), branch to the inline path
     (`0x80668d44` / `0x8028a54c`) instead of `CallCommands`. The stale secondaries are dropped.
3. **Writes.** The table is written only at the call sites (layer-list workers) and read at job entry. Each entry is one
   atomic word set. INFERRED: the next frame's jobs start only after this frame's graph has joined (`f_80070b20` waits for
   every worker), so the six jobs of a pass always see the same answer.

**What the job gets: formats and sample count only.** The image identity is not needed by a secondary, and it does not
exist yet (section 2). The layer list's `vkCmdBeginRendering` gets the images from its SetRenderTargets, through
`rt_slots.c`.

**What cannot be known in time.** The formats of a key the first time it appears in a session, and after a settings change
that alters a target format. Both cases are exact anyway, because they take the inline path. Expected cost: one frame of
serial recording for each new key, about 23 keys per scene (e4: 23 records in Kokiri, with the cube
refilter), once per session if the key objects persist, plus one frame per format-changing settings change. No job is
skipped and no job runs against the wrong formats.

## Decision for the plan

Replace "pass table keyed by (view record, key word 0), filled by earlier frames; skip on a miss" with a table keyed by the
**key pointer**. Fill it at the two job-call sites from the layer list's own targets. Use the game's inline path
(`f_80681320`) for a miss, so a key not yet seen and a format change are recorded inline on the layer-list thread rather
than skipped. The game patch is two splices plus the existing recorder entry hook. Moving target allocation before the kick
is not needed. M0 still owes:

- (a) one run forcing every key inline, to confirm by pictures that `f_80681320` is complete;
- (b) a key-pointer log over the scene cycle, to confirm that the pointers persist and the formats per key are constant.
