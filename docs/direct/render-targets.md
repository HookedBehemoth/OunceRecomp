# Render targets

This page explains where the game's render targets come from, what sets their sizes and formats, which switches change
them, and where the host can take control. Read it before you change an output size, the shadow resolution, the
upscaling path or `games/oot/patches/rt_slots.c`. How transient targets get device memory is in
[the transient pool](transient-pool.md).

## Sizes

Four sizes decide every target:

| Size | Meaning | Default |
|---|---|---|
| Output size `O` | the display size the host tells the game (`hos_output_size`, `runtime/nn_applet.c`) | 2560 x 1440 |
| Frame size `F` | the picture the game composes: `O` times the game's graphics scale (0.8 in output mode 0, otherwise 1.0) | 2048 x 1152 |
| Scene size `S` | the size the 3D renders at: 3/4 of `O`, times the graphics scale, times the 3D scale `s` | 1536 x 864 |
| 3D scale `s` | the game's dynamic-resolution factor; fixed at 1.0 unless dynamic resolution is on | 1.0 |

The window's own size does not reach the game. The present blits the window texture, cropped as the game asks, into the
swapchain, so a window resize only remakes the swapchain.

### Switches

| Switch | Effect |
|---|---|
| `HOS_OUTPUT_RES=<n>` or `output_res=<n>` in `ounce_options.ini` | `O`: 0 the game's 2048 x 1152 frame, 1 2560 x 1440, 2 3840 x 2160, 3 1920 x 1080, 4 5120 x 1440 |
| `HOS_SCENE_NATIVE=1` | the scene's base scale 1.0 instead of the game's 0.8 |
| `HOS_RES_SCALE=<s>` | the fixed 3D scale, up to 2.0; above 1.0 the frame is rendered larger and taken down (Upscaling) |
| `HOS_DYNRES=1` | the game's own dynamic resolution, driven by GPU time |
| `HOS_RT_POOL_MB=<MB>` | the game's render-target pool; by default it scales with `O`'s pixels and is at least 512 MB |

A change of `s` reaches the targets on its own: the next frame's descriptors differ, so the allocator makes new targets.
The window textures, `F` and the UI layer are made once at boot; the game does not support changing them in play.

## The allocator

Every render target the game makes in play comes from agl's dynamic texture allocator (`dta_alloc` in
`games/oot/labels.txt`). Its descriptor carries the target's name, format, type, size, depth or layers and levels. A
flag marks a target as transient: it is freed within the frame (`dta_free`). Without the flag it is persistent.

The host hooks the allocator in `games/oot/patches/rt_slots.c`:

- Transient targets get a stable slot each, so a logical target lands at the same address every frame. In direct mode
  the slots are the [transient pool](transient-pool.md).
- Local shadow maps get their size rounded up to a size class (below).
- Persistent targets are left to the game.

To log every slot made, with its name, size and format, run with `HOS_RT_SLOTS_LOG=1`.

## What the targets are

| Kind | Examples | Size |
|---|---|---|
| scene targets | the lit color (R11G11B10F), depth (D32F), SMAA, SSAO, fog, depth probes, the shading-rate image | `S`, `S`/2, `S`/4 or `S`/16, from the view rectangle |
| fixed targets | bloom and depth-of-field chains, the bilateral grid, the 256 x 144 occlusion depth, the cube-map depth | constant |
| output-derived | `DepthBuffer` and `Reprojection` | `O`/4 |
| shadows | sun cascades, local-light maps, the static sun shadow | from the scene config and per light (below) |
| movie and UI | the movie planes, `NDVE_Output`, UI captures | the movie's or the layout's size |
| persistent | window textures (`O`), the UI layer and the frame (`F`), the static shadow and far-depth maps | made once |

At the defaults the transient targets peak at roughly 300-350 MB; at `O` = 3840 x 2160 near 900 MB.

The frame is forward-rendered: a depth prepass, then color draws into the lit R11G11B10F target. The RGBA8 scene-sized
target is a screen-space mask, not a normals target.

## Shadows

| Kind | Size comes from | Notes |
|---|---|---|
| sun cascades (`shadow_map_depth`) | the scene config `gsys/ModelSceneConfigParam/Main.gsys__ModelSceneConfigParam.bgyml` (`DepthShadowTexWidth`, ...) | 1600 x 1600 x 3 layers of D16 in Kokiri and Hyrule Field |
| local lights (`local_shadow_depth`) | set per light each frame from its screen size or distance, scaled by `LocalDepthShadowParam` | square, 384 to about 2,400; one per shadowed light per frame |
| static sun shadow | the scene config | 12288 x 12288 D16, persistent |

A local shadow map lives for one frame: it is allocated when the local-shadow pass records, sampled by the deferred
lighting in the same frame, and freed at the view's last pipeline stage. Its viewport comes from the target's own
width, so a larger map is drawn whole. The host rounds each light's map up to a size class (256, 384, 512, 640, 768,
1024, 1280, 1536, 2048, 2432) and keeps a light in its class until it needs a larger one or falls to 90% of the class
below. That keeps the number of distinct shapes small. `HOS_RT_QUANT=0` turns the classes off.

Shadow filter widths are in texels, so a larger map gives a narrower penumbra unless the filter scales with it.

## Upscaling

Both games keep agl's NDSR stage with its ratio tables (`games/oot/patches/ndsr.c`, `games/dkb/patches/ndsr.c`). Each
of OoT's networks lists the output-to-input ratios it supports (1.0 and 1.2 up to 3.0), and the stage picks the ratio
nearest the 3D scale. After the scene, lighting, fog and post effects:

1. The lit colour is copied for anti-aliasing (`color(AA)`) at the render size.
2. When the render size is the output's, SMAA resolves straight into the frame, and no upscaler runs.
3. Otherwise SMAA resolves into `ndsr::smaa_output` (RGB9E5F) at the render size, holding the square root of the colour,
   and the chosen upscaler takes it to the frame.
4. The UI is composed over the frame into the window texture.

The upscaler is a choice that can change while the game runs (`runtime/upscale.h`):

| Choice | What runs | 3D scales |
|---|---|---|
| game | the game's network, a few layers of matrix multiplies | up to 1.0 |
| bilinear | the host's resample with a tent filter | up to 2.0 |
| Catmull-Rom | the host's resample with a Catmull-Rom filter, clamped against ringing | up to 2.0 |

The game's network cannot take a frame down, so the settings offer it only at scales up to 1.0. A kind 2 or 3 network
also runs at ratio 1.0 as the game ships it, a 1:1 pass over SMAA's output; the patch skips it, because it costs about
1.3 ms a frame at 2560 x 1440.

Above 1.0, the scene renders at the output size times the 3D scale, and the host's filter takes it down with a kernel
widened by the ratio, so every rendered pixel counts. The render target pool and the game's memory grow with the
square of the scale at boot, so a scale above the boot scale needs a restart.

The host's filters are one back-end op that records a compute pass into the game's command buffer, in the network's
place. The game provides no motion-vector target and no target that carries colour across frames, so a temporal
upscaler (FSR3, XeSS, DLSS) also needs:

- depth (`RawDepth(Full)`, D32F, 0 at the near plane), which exists at the render size;
- motion vectors, which can be derived from depth and the two frames' view-projection matrices;
- a jittered projection, which the game does not apply.

## Things to keep consistent

- **A format change must reach the jobs.** A render job's pass formats come from the targets its layer list sets
  ([render jobs](jobs.md)); change a format at allocation and the key's table entry follows, but only if the shaders
  write and sample the new format the same way.
- **Some passes size their viewport from the view rectangle**, not from the target. Making such a target larger at the
  allocator alone leaves part of it unused. Only passes that read the target's own size, like the local shadows, can
  be resized at the allocator.
- **The pool scales with `O` only**, while scene targets grow with `S`. A large `S` at a small `O` needs
  `HOS_RT_POOL_MB`.
