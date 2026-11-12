# M4f report: the remaining picture differences in Kokiri (2026-11-12)

Branch `agent/m4f` (from native-vk bc3eb14). Runs `work/runs/m4f_*` (local), RX 6950 XT, RADV, `RUN=timing`, one game
process at a time, Kokiri `chisel_kokiri`, HF `chisel_hf`. **No device was lost.** Tests: 292 pass / 5 fail (the baseline).
Reference: a frozen run of this same build, `m4f_fd_k` (`RX_VK_DUMP=1800`; its shot equals `m2b_fd_kokiri_b`'s).

## 1. The cause of the x3.96, the green cast and the light shaft: memory read only through stored pointers was unbound

**Evidence.** `HOS_DBG=envlog` (`host/direct_diag_env.c`, either back end) logs what the exposure reader `f_80357060`
reads. Frozen (`m4f_env_f`) against direct (`m4f_env_d`) at present 1800: its first and third GPU sums agree (253.8 / 251),
the second, at `0x113c88e000`, is **38.3 22.6 8.4 25.7 frozen, 0 0 0 0 direct**, and `[[[0x84c9e740]]+0x1e8]+0xdac` (the
exposure the LUT pass's light words carry) is **0.005578 frozen, 0.022097 direct: x3.96**. The intensity was a
consequence, not a cause. That sum is written by compute `435a0138b9013040` (IR via tmtool: 256 samples of SH irradiance
from light probes, atomically added). Its threads' results were 0 in RGB. The trace showed why: its pointer table
`s2 = 0x108c6d6580` holds `+32 0x108e878680`, the probe data, **not bound in layer 0**: 26 MB into a 150 MB pool
(`0x108c6e8000`, flags 0x24). Layer 0 bound pools up to 64 MB ahead and larger ones only where a binding named a range; a
range reached only through a pointer in memory read the null window: zeros. 11 programs read that pointer, the froxel
fog/light volumes (`27ee9883`, `ee7764a6`, `755a777a` x9) among them.

**Fix** (`direct_pools.cpp`, review note applied, commit 240f9d0): every pool the GPU can read is bound in full when it is
made, whatever its size. Not bound: GPU_NO_ACCESS, virtual and shader-code pools, and the texture data pools, named by
identity: the host names each pool `agl::TextureDataMgr` makes (its `nvnMemoryPoolInitialize` returns to `0x80f6cb20`,
`HOS_NVN_LOG_POOLS=1`, run `m4f_pl_d`) through a new seam call `hsd_texture_data_pool` before layer 0 sees it (Kokiri: 2432,
201 and 2 MB). One log line per pool of a chunk or more, the rest counted. Cost, per run:

| Run | chunks bound | imports / binds | longest | RSS at 44 s | first job draw after the device |
|---|---|---|---|---|---|
| Kokiri, 64 MB rule (`m4f_tr2_d`) | 188 (3008 MB) | 826 / 120 ms | 124 ms | 5341 MB | 15.2 s |
| Kokiri, all pools (`m4f_full2_k`) | 174 (2784 MB) | 749 / 88 ms | 172 ms | 5277 MB | 15.1 s |
| HF, 64 MB rule (`m4a_dd_hf`, older build) | 171 (2736 MB) | 849 / 106 ms | 128 ms | 4859 MB | |
| HF, all pools (`m4f_full_hf`) | 160 (2560 MB) | 745 / 115 ms | 161 ms | 5035 MB | 15.4 s |

Fewer chunks than before: shader-code pools (64 MB and under, bound ahead before) and the 201 MB texture data pool are left
out. Pools bound in full at creation: 4320 (2134 MB) Kokiri, 4141 (2053 MB) HF. No pool failed to bind.
**VRAM-class pools** (CPU_NO_ACCESS) can hold pointed-to data: their interior granules are in VRAM from their creation
(`vram_pool_made`), their partial edge granules are host memory and are now bound with the rest. **Virtual pools** have GPU
addresses past layer 0's window: a pointer to one reads the null window. Their physical pool (`0x100dc17000`, 512 MB) is
GPU_NO_ACCESS. None was seen read (the trace's pointer check lists no unbound pointer after the fix).

**Result** (`m4f_ahead_d`, `m4f_fin_k`): in2 and the exposure match the frozen run (38.24 22.65 8.39 25.68; 0.005574); the
256x256 LUT pass 36.9 -> 0.03; the sky 65.4 -> 0.04; RGBA8 384x216 13.2 -> 8.3-11.5. The large ivy leaves on the house and
the right wall now draw yellow (c), the second dark pot draws (b), the green cast and the bright blue light shaft are gone
(e). The picture is now darker than the frozen one (lit target mean 0.012 against 0.022; composite 0.037 against 0.079).

## 2. What still differs, object by object (`m4f_fin_k/shots/vk_001800.png` against `m4f_fd_k/shots/vk_001800.png`)

| Object | Cause (evidence) | Fixed |
|---|---|---|
| The giant tree's bark walls behind and around the house are black; the floor's grass | **task+mesh draws** (15 a frame skipped, M4c): the frozen-only programs `6ea0c171`, `974ea2c9`, `636eed4d`, `a03bdab9`, `507e946d`, `da7a7ab2` (direct mesh, the `2c56` meshlet family) and `28dd2baa`, `8a8304ed`, `43ec86f3`, `51183fe7`, `8e2f7af4` (indirect, lists `0x11789xxxxx`) | no (M4c) |
| Overall darkness (lit 0.54x) | the missing walls and grass are the brightest large surfaces of the frozen frame; the env cube refilters from this darker frame (128x128 mean 0.0086 vs 0.0156) | follows M4c |
| (f) The top-left opening shows bright lit leaves | the tree wall in front of them is task+mesh, missing; the leaves behind are drawn and lit | follows M4c |
| Extra draws: direct ~1070 a frame, frozen 685; 43 programs drawn only in direct, mostly jobs into 1536x864 | objects behind the missing walls pass the GPU culling and the occlusion tests (consistent with `qfixed,qcputs`: 250 programs, unchanged); shadow cascades hold more casters | follows M4c |
| (d) Link's hair, hat and face | his model's textures `Player_Child_Hairs_Alb/Nrm/Rgh/AO/Thc`, `Mt_Cloth_Parts_Player_Child_Rgh/AO` and others are **"loaded with no data and in no BNTX: a plain image"** (BaseProcCreateAndDeleteThread). The image is never written ("written never" in the trace), so its memory is undefined: speckled noise in `m4f_ahead_d`, dark in `m4f_fin_k`. What fills them on the console (frozen: from guest memory) is not found | no |
| (b) Two left pots draw but dark, the basket at the right is not visible | the pots are dark because the walls that light them by bounce, and the env cube, are dark. The basket's draw was not identified. Frozen-only non-mesh programs (`5fbaa372` x36, `12daa3fb` x20, `62ac9d55`, `a14c2ba4`, `154f1dcf`, `4c9a9102`, ...) did not appear at frame 1800 of the frozen per-draw dump (`RX_VK_DUMP_EACH`, `m4f_feach_k`), so they come and go between frames | open |
| RGBA8 384x216 `0x10012df0000` (B flat 0.72 vs dithered 0.85), sampled by every lit program | the 64^3 BC4 volume `0x1140bcb000` that `531432c3` samples: zero in **both** back ends' guest memory (`envlog`, frozen `m4f_fwatch2_k`), no CPU write in the frozen run either (page watch `RX_NVN_WATCH_TEX_AT`, new), no dispatch or copy writes it in direct (`HOS_DBG=writer`, new), no texture copy is skipped (new log). The frozen dither therefore does not come from that volume's texels; the other input, the 1360x1000x8 R16 store texture `0x10b19c0a00`, is the next suspect | open |
| Shadow cascades D16 1600x1600 (16.0) | the aligned frozen row is its cleared state; the frozen final states hold fewer casters (above) | n/a |

## 3. The per-draw trace (built)

- **Direct** (`HOS_DBG=drawlog:<present>`, off by default, nothing done when off): every draw of the frame, in recording
  order per thread, with list kind and return address, pass size and format, program key and **stage code keys**, vertex
  layout and buffers, uniform buffers, **storage buffers and the pointers a small one holds (flagged "not bound in layer
  0")**, every texture with format, size, what wrote it and **its store name**, dynamic state, and **skipped draws of every
  list kind with their reason and program**; **every dispatch** with grid, buffers, textures and images. Lines past 2 KB are
  split (`drawlog+`). `HOS_DBG=writer:<hex>`: dispatches and buffer copies over an address, at any time.
- **Frozen**: `RX_VK_DRAW_STATS=<dir>` (touch `<dir>/draw_stats.now` once in play) gives per-program draw counts.
- `scripts/direct_trace_compare.py <frozen draw_stats_N.txt> <direct log.txt>`: programs drawn or dispatched on one side
  only, and per-program count differences. Program keys agree between the back ends (201-207 of 226 drawn, 74 of 75 dispatched).
  The frozen counts miss draws at its frame boundary, so "frozen only" is the reliable list.

## 4. Per pass after the fixes (`m4f_fin_k` vs `m4f_fd_k`, 126 aligned, median 0.04, mean 1.28)

Within noise: depth, shadow-depth chain, LUT (0.03), sky (0.04), the 768x432 / 384x216 chains, bloom chain (0.2-0.7).
Beyond noise: lit B10G11R11F 1536x864 5.9-18.8 (walls and grass, section 2); RGBA8 1536x864 13.2 (grass);
RGBA8 384x216 11.5 (the flat B channel); composite 8.0 (darker: task+mesh).

## Open problems

Task+mesh (walls, grass: most of what remains, M4c); Link's no-data textures; the 384x216 dither source; the basket.
Plain images that are never written should be defined (cleared at creation) so they cannot show stale memory.

## Shot pair

`work/runs/m4f_fin_k/shots/vk_001800.png` (direct), `work/runs/m4f_fd_k/shots/vk_001800.png` (frozen, same build).
