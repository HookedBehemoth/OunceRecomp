# Render targets: sizes, formats and where to take control

This page lists the render targets the game creates, where each one's size and format come from, how the window's size
reaches them, and where the host or the back end can decide them instead. The last two sections are a proposal: render
at the window's size times a bias with an upscaler of ours, configurable shadow resolution, and a host-side policy for
any target.

Guest addresses are those of the supported build (HOS_GAME_VERSION 100); their labels are in `nx2/oot_labels.txt`.
**Measured** means read from a run's log; **read** means read in the code at the given location; **inferred** means
concluded from shapes, names or numbers and not confirmed.

## Terms

| Term | Meaning | Where it is set |
|---|---|---|
| Output size `O` | The size the host tells the game its display is: `hos_output_size` | `host/nn_applet.c:127` |
| Frame size `F` | The size of the picture the game composes (3D upscaled, then UI). `O` times the graphics scale factor (mode 1: 0.8 when the output mode is 0, otherwise 1.0) | `f_806702e0` `display_scale_mode_select` |
| Scene size `S` | The size the 3D renders at. `BaseResolution` (3/4 of `O`, scaled with it) times the graphics scale factor times the 3D scale | `f_80ea2f30` `display_base_resolution`, `f_831f3200` `scene_resolution_update` |
| 3D scale `s` | The module field `+0xe5c` (`= requested * (+0xe58) / (+0xe50)`); 1.0 on the fixed path | `host/nn_vi.c:230-275`, `host/oot_probes.c` (`hos_oot_scale_recompute`) |
| Window | The SDL window and its swapchain; any size | `gpu/direct/direct_present.cpp:385` |

With the defaults (`output_res=0`, fixed path, `s = 1.0`): `O` = 2560 x 1440, `F` = 2048 x 1152, `S` = 1536 x 864
(measured: run logs `vi: 3D scale 1.000 (+0xe50 1152 2048 1152)`, census below). With `HOS_OUTPUT_RES=2`: `O` = `F` =
3840 x 2160, `S` = 2880 x 1620 at `s = 1.0`.

## The allocator

Every render target the game makes in play comes from one function: `f_8005ed00` `dta_alloc`
(agl::utl::DynamicTextureAllocator, instance at `0x84ea1b80`). Its third argument is a 0x38-byte descriptor on the
caller's stack: `+0` name, `+8` agl format, `+0xc` type, `+0x10` width, `+0x14` height, `+0x18` depth or layers, `+0x1c`
levels, `+0x30` flags. Flag bit 0 of the fourth argument marks a transient target (freed in the frame by `f_802737a0`
`dta_free`); without it the target is persistent. The agl format indexes the game's table at `0x83f4848c` (u32 NVN format
per agl format, 0x5f entries; `host/rt_slots.c:538-553`).

The host already hooks the allocator: `host/rt_slots.c:461` (`ENTRY_HOOK(0x8005ed00)`) and `alloc_pre`
(`host/rt_slots.c:476`) read the descriptor, and for local shadow maps rewrite its width and height before the lifted body
runs (`host/rt_slots.c:493-500`, the shadow size classes). The pool's slots and the back end's transient pool are in
[transient-pool.md](transient-pool.md).

## Inventory

Census: every transient target the allocator made in two runs with `HOS_RT_SLOTS_LOG=1` (`rt slots: new <name> <w>x<h>x<d>
fmt <agl> lr <caller>`): `work/runs/m6s_c1` (output mode 0, `F` = 2048 x 1152, dynamic resolution on) and
`work/runs/pic4_4k` (mode 2, `F` = 3840 x 2160, dynamic resolution on). Both varied `s`, which separates the targets that
follow `S` from those that do not. Formats are the agl format decoded through the game's table. Callers are the
functions holding the allocation's return address.

### Scene-sized targets (follow `S`)

| Name | Format | Size | Allocated by |
|---|---|---|---|
| `gsys::RenderBuffer::color` | R11G11B10F | `S` | `f_806790d0` `pass_scene_depth_helper` |
| `gsys::RenderBuffer::color(AA)` | R11G11B10F | `S` | `f_802707b0` `pass_aa_color` |
| `gsys::RenderBuffer::SoftEdgeDepth` | DEPTH32F | `S` | `f_80666d20` `pass_soft_edge_depth` |
| `RawDepth(Full)` | DEPTH32F | `S` | `f_8067aa20` `pass_raw_depth_chain` |
| `Nld(Full)` | R32F | `S` | `f_80679b70` `pass_nld_chain` |
| `Depth For FogPrePass` | DEPTH32F | `S` | `f_8028a7f0` `pass_fog_prepass_depth` |
| `skin_shading` | R11G11B10F | `S` | `f_8028ca00` `pass_skin_shading` |
| `shadow_prepass_light_buffer` | RGBA8 | `S` | `f_8065e180` `pass_shadow_prepass` |
| `ScreenSpaceShadow` | R8 | `S` | `f_80664830` `pass_screen_space_shadow` |
| `ndsr::smaa_blendweights` | RGBA8 | `S` | `f_80271be0` `pass_smaa_edge` |
| `ndsr::smaa_output` | RGB9E5F | `S` | `f_802715b0` `pass_smaa_output` |
| `gsys::RenderBuffer::Color(half)` | R11G11B10F | `S`/2 | `f_802a3700` `pass_half_color` |
| `RawDepth(Half)`, `Nld(Half)` | DEPTH32F, R32F | `S`/2 | `f_8067aa20`, `f_80679b70` |
| `agl::utl::DepthBufferEx::HalfNld(Max)` | R16F | `S`/2 | `f_802a48b0` `pass_half_nld_max` |
| `FogPrePass::HalfDepth` | DEPTH32F | `S`/2 | `f_80289590` `pass_fog_half_depth` |
| `FogPrePass AfterWater`, `BeforeWataer` (sic) | RGBA16F | `S`/2 | `f_8028b110` `pass_fog_water` |
| `ReducePrePass` | RGBA16F | `S`/2 | `f_8027a310` `pass_reduce_prepass` |
| `Fluid 2dVolume New Depth` | DEPTH32F | `S`/2 | `f_802ad020` `pass_fluid_depth` |
| `ssao_work`, `ssao` | R8 | `S`/2 | `f_80660010` `pass_ssao_work`, `f_80663c20` `pass_ssao` |
| `Blur0`, `Blur1`, `Reduce` | RGBA8 | `S`/2 | `f_8066bec0` `pass_deferred_lighting` |
| `Blur0`, `Blur1`, `Reduce` | R11G11B10F | `S`/4 | `f_8028ca00` `pass_skin_shading` |
| `merge` | RGBA32F | `S`/4 | `f_80299510` `pass_closest_surface` |
| `depth`, `closest_surface_depth`, `closest_front_depth` | DEPTH32F, R32F, R32F | `S`/4 | `f_80299510` |
| `closest_front_info`, `addition` | R16F, RG16F | `S`/4 | `f_80299510` |
| `RawDepth(Quat)`, `Nld(Quat)` | DEPTH32F, R32F | `S`/4 | `f_8067aa20`, `f_80679b70` |
| `inner_depth_tex`, `outer_depth_tex`, `inner_depth_probe_tex`, `outer_depth_probe_tex` | DEPTH32F | `S`/4 | `f_802867f0` `pass_depth_probes` |
| `inner_mask_probe` | RG32F | `S`/4 | `f_802867f0` |
| `wet_mask` | RGBA8 | `S`/4 | `f_80669b80` `pass_wet_mask` |
| `ndsr::smaa_edge` | RG16UI | `S`/4, rounded up | `f_80271be0` |
| `ndsr::smaa_edge_list` | RGBA8 | 2048 x ceil(`S` area / 8192 ...) | `f_80271be0` ([terrain-texels.md](terrain-texels.md), site 13) |
| `shading_rate_image` | R8UI | `S`/16 | `f_8065f190` `pass_shading_rate_image` |

`f_80299510` takes its size from the view rectangle, `(rect.x1 - rect.x0) >> 2` of the view at `+0x3a10`
(`work/assets/native/rt-alloc.md` 2): the scene-sized targets follow the view, not a size stored with the target.

### Targets of fixed or output-derived size (do not follow `S`)

| Name | Format | Size | Derives from | Allocated by |
|---|---|---|---|---|
| `DepthBuffer` | DEPTH32F | 640 x 360 at `O` = 2560 x 1440; 960 x 540 at 3840 x 2160 | `O`/4 (measured; constant while `s` changed) | `f_8065c660` `pass_depth_buffer` |
| `Reprojection` | R11G11B10F | as `DepthBuffer` | `O`/4 (measured) | `f_8065c660` |
| `detect0`, `detect1` | R11G11B10F | 480 x 270 down to 30 x 16 | constant (same at both outputs) | `f_8026a9e0` `pass_bloom_detect` |
| `blur` | R11G11B10F | 240 x 135 down to 30 x 16 | constant | `f_80267200` `pass_bloom_blur` |
| `dof_blur_mipmap`, `dof_blur_mipmap_weight` | R11G11B10F, R8 | 960 x 540 | constant | `f_80280270` `pass_dof` |
| `BilateralGrid`, `bilateral_grid_tmp` | RG16F | 24 x 16 x 64 | constant | `f_802754d0` `pass_bilateral_grid`, `f_808bf670` `pass_bilateral_grid_compute` |
| `agl::utl::DepthBufferEx::OcclusionDepthBuffer` | DEPTH32F | 256 x 144 | constant (the GPU occlusion test) | `f_802ad740` `pass_occlusion_depth` |
| `render_cubemap_depth` | DEPTH32F | 128 x 128 | constant | `f_8025eb40` `pass_cubemap_depth_alloc` |
| `SnapshotColor`, `SnapshotStrokeTexture`, `SnapshotDepth` | RGBA8, RGBA8, DEPTH32F | 512 x 512, 1024 x 1024 | constant | `f_8065afe0` `pass_snapshot` |
| `shadow_map_depth` | DEPTH16 | 1600 x 1600 x 3 (and x 1) | the scene config (Shadows) | `f_80669460` `pass_sun_shadow_alloc` |
| `local_shadow_depth` | DEPTH32F | square, 384 to 2432 | per light (Shadows) | `f_802640a0` `pass_local_shadow_depth` |
| `MovieRenderer` | R8 and RG8 | 1920 x 1080 and 960 x 540 | the movie's size | `f_83b82250` `movie_renderer_copy_frame` |
| `NDVE_Output` | RGBA8_SRGB | 2560 x 1440 at both outputs | constant | `f_83b83360` `movie_renderer_draw_ndve` |
| `SaveLoadIcon_00`, `PaBalloonMessage_00`, `PaGuideKeyCapture_00`, `DoCommand_00`, `MapMenu_00` | R8 (`MapMenu_00`: R11G11B10F 400 x 400) | about 100-250 px | the UI layout | `f_80792270` `ui_pane_capture_alloc` (and others) |

### Persistent targets (not in the slot census)

Persistent allocations take the allocator's top-down path (`f_80062c50`) and are left to the game by `alloc_pre`
(`host/rt_slots.c:491`). From `work/assets/native/rt-alloc.md` 2 and `engine-frame.md` (read and measured there):

| Target | Format | Size | Derives from |
|---|---|---|---|
| Window textures (3) | RGBA8 | `O` (2560 x 1440 by default) | `nnMain` (`f_81222ce0`) puts the display size in the framework's create argument; `hos_oot_output_size` (`host/oot_probes.c`) |
| `agl::RenderDisplay` colour and depth | - | `F` (inferred) | `f_809ed030` `agl_render_display` |
| UI layer texture | R11G11B10F | `F` (2048 x 1152 by default) | the display-size object |
| Upscale network output (the frame) | R11G11B10F | `F` | the network's output (`gpu/legacy/vk/vk_draw.cpp:6400-6445`: input RGB9E5F at `S`, output R11G11B10F at `F`) |
| `copy_color_buffer`, pause texture, screenshot / capture | - | `F` (inferred) | `f_80697370` `pass_copy_color_buffer` |
| Static sun shadow, far depth | DEPTH16 12288 x 12288; DEPTH32F, R32F, DEPTH16 4096 x 4096 | constants (inferred) | `f_809ec950` `pass_static_shadow_depth`; [shadow-maps.md](shadow-maps.md) 1.1 |
| `shadow_map_reduce_depth`, `vsm`, `vsm_temp` | - | - | conditional (rt-alloc.md 2) |

At the defaults the transient targets peak at 291-340 MB of the 512 MB transient pool (measured: the `rt slots: transient
pool` lines of `work/runs/sa_w8`, `sb_base`, `warmsh2`), and 884 MB of 1374 MB at `O` = 3840 x 2160 (`work/runs/slots_r2`).

## Size pipeline

1. **Output size.** `hos_output_size` (`host/nn_applet.c:127`) returns `O` from `output_mode` (`host/nn_applet.c:116`):
   `HOS_OUTPUT_RES`, else `output_res=` in `hookshot_options.ini`, from a fixed list (0: 2560 x 1440 with the game's 0.8,
   1: 2560 x 1440, 2: 3840 x 2160, 3: 1920 x 1080, 4: 5120 x 1440). Handheld forces 0. `nn::oe::GetOperationMode`
   answers docked (`host/nn_applet.c:154`).
2. **Window textures.** `nnMain` (`f_81222ce0`) writes `O` into the framework's create argument (`+20`, `+24`); the
   framework makes 3 window textures of that size (`f_8348be50` `sead_framework_graphics_init`). Edit 0 of `OUT_EDITS`.
   With a screen wider or narrower than 16:9, `hos_wide_out` replaces it (a `PATCH_AT` row in `host/oot_probes.c` `hos_oot_wide_out`, `host/hos_main.c:1555-1566`:
   the height stays `O`'s, the width follows the screen's shape, capped at 3840 x 2160's pixels).
3. **Display-size object.** `f_810a7b70` `display_size_setup` takes a config's docked size list; its first entry
   (2560 x 1440) is replaced by `O` (edit 1). It becomes the frame's base (`B+244`).
4. **Graphics scale.** `f_806702e0` `display_scale_mode_select` picks a scale mode from a table (1.0, 0.8, 2/3) per
   profile; the game runs mode 1. `F` = base times the factor. With output mode 0 the game's answer stays
   (`F` = 2048 x 1152); explicit sizes use 1.0 (`host/nn_applet.c:112-115`). `HOS_SCENE_NATIVE=1` turns mode 1's 0.8 into 1.0
   (`host/oot_probes.c` `hos_oot_scene_mode`).
5. **Scene base.** `f_80ea2f30` `display_base_resolution` reads ResolutionParam's `BaseResolution` (1920 x 1080 as
   constants, 3/4 of 2560 x 1440); edit 2 scales it with `O`, edit 3 writes `O * 3/4` as floats into the module's `+0xe54` and `+0xe58`
   (the sizes the 3D scale is taken against). `hos_oot_wide_scene` (`hos_wide_scene`, a `PATCH_AT` row in `host/oot_probes.c`) replaces it for a non-16:9 screen.
6. **3D scale.** `f_831f3200` `scene_resolution_update` computes `+0xe5c = requested * (+0xe58) / (+0xe50)` when `+0xe2c`
   bit 3 is set. The dynamic path (the engine's flag `+0x94`, bit cleared by `hos_oot_dynres_flags`, `host/oot_probes.c`) follows
   the GPU time; the fixed path takes `hos_fixed_scale_bits` (`hos_oot_fixed_scale`), and `hos_oot_scale_recompute`
   asks for the recompute only while `+0xe5c` differs. `hos_fixed_resolution` (`host/nn_vi.c:238`) selects the fixed
   path unless `HOS_DYNRES=1`; `HOS_RES_SCALE` sets the scale (`host/nn_vi.c:258`). The patch accepts scales up to 2.0.
7. **Scene targets.** Each pass function allocates its targets while it records, sized from the view rectangle
   (`S`, `S`/2, `S`/4, `S`/16; table above). A change of `s` makes the next frame's descriptors differ, so the allocator
   makes new targets; nothing else has to be told (measured: the dynamic path moves `s` between 0.333 and 0.75 in play,
   `work/runs/flash1`).
8. **Upscale.** SMAA writes `ndsr::smaa_output` (RGB9E5F, `S`); the game's upscale network (compute program
   `fd029a317a9a4b94` at `S` = 3/4 `F`, one program per fixed share) writes the frame (R11G11B10F, `F`). See Upscaling.
9. **Pool.** The render-target pool is the config's `+1464` (0x13100000 bytes) read by `f_836ce9d0`
   `dta_pool_config_read`; edit 4 replaces it with `hos_rt_pool_size` (`host/nn_applet.c:141`: scaled with `O`'s pixels or
   `hos_wide_out`'s, at least 512 MB, `HOS_RT_POOL_MB` over it); edits 5-7 grow the Graphics heap to match.
10. **Present.** The game composes the UI over the frame into the window texture (`f_80285b00` `pass_final_blit`) with
    a crop (`nvnWindowSetCrop`); `present_run` (`gpu/direct/direct_present.cpp:421`) blits the crop into the swapchain,
    linear filter (`:492`). A window resize remakes the swapchain only (`present_resize`, `:385`).

## Shadows

| Kind | Size and count come from | Memory |
|---|---|---|
| Sun cascades `shadow_map_depth` | `f_80669460` reads width `+0x70`, height `+0x90`, layers = cascade count, format `+0xf0` (0x5a DEPTH16 or 0x5c DEPTH32F) ([shadow-maps.md](shadow-maps.md) 1.1). The scene config `gsys/ModelSceneConfigParam/Main.gsys__ModelSceneConfigParam.bgyml` has the keys `DepthShadowTexWidth`, `DepthShadowTexHeight`, `DepthShadowCascadeNum0`, `DepthShadowNear00`-`03`, `DepthShadowFar0`, `DepthShadowNearFarMargin`, `DepthShadowPolygonOffset`, `DepthShadowPolygonScale`, `DepthShadowPcfOffset`, `DepthShadowForceArray` (inferred: the names match the measured 1600 x 1600 x 3; the values were not decoded) | W x W x N x 2 bytes: 15.4 MB at 1600 x 3; 25 MB at 2048 x 3; 101 MB at 4096 x 3 |
| Local lights `local_shadow_depth` | side = shadow `+0x4e8` (light `+0x5e8`), set per light from its screen size or distance, scaled by `LocalDepthShadowSetting` `ResolustionMaginify` (sic) in `System/GameGraphics/LocalDepthShadowParam/Default.game__gfx__LocalDepthShadowParam.bgyml`; the distance-to-size code is not found ([shadow-maps.md](shadow-maps.md) 1.3) | side^2 x 4 bytes: 23.7 MB at 2432; one per shadowed light per frame |
| Static sun shadow, far depth | `f_809ec950` (square `+0x80c`, format `+0x800`); the config's `StaticSdwDepthFormatName`, `StaticSdwShadowMapFormatName`, `StaticSdwShadowMapType` | 12288^2 x 2 = 302 MB; three 4096^2 maps 64 + 64 + 32 MB (persistent) |
| Shadow pre-pass and screen-space shadow | `S` (table above); per-area settings in `agl/ShadowPrePassParam/*.bgyml` (`ResolutionMode`, `PcfWidth`, `MipBlurWidth`, `ScreenSpaceBlurWidth`, `IsUseStaticDepthShadow`) | follows `S` |

What depends on the shadow resolution: the PCF filter width and offsets (`DepthShadowPcfOffset`, `PcfWidth`, the
`LocalDepthShadowSetting` `PcfType` and `BlurWidth`) are in texels, so a larger map gives a narrower penumbra unless they
scale with it; the depth bias (`DepthShadowPolygonOffset`, `DepthShadowPolygonScale`) is in depth units and does not
follow the texel size. The cascade fit is done on the CPU from the view's frustum (docs/parked-patches.md, the camera jitter). The
local map's viewport comes from the target's own width (`f_802640a0`, [shadow-maps.md](shadow-maps.md) 2, step 2), so a
larger target is drawn whole; rt_slots already rounds it up (`host/rt_slots.c:493-500`). Whether the sun cascades' viewport
also comes from the target was not checked.

## Upscaling

### The game's path

| Step | Pass | Input | Output |
|---|---|---|---|
| Scene, lighting, transparent, fog, DOF, bloom | `f_80660320` list (engine-frame.md rows 8-32) | | `gsys::RenderBuffer::color` R11G11B10F `S` |
| Copy for AA | `f_802707b0` `pass_aa_color`, `f_80697370` | colour | `color(AA)` R11G11B10F `S` |
| SMAA | `f_80270da0` `pass_smaa`: edges and weights (`f_80271be0`), resolve (`f_802715b0`) | `color(AA)` | `ndsr::smaa_output` RGB9E5F `S` |
| Upscale network | compute program `fd029a317a9a4b94` (about 13 passes over storage buffers, matrix multiplies); networks made by `f_81503ce0` `ndsr_networks_init` from `agl_ndsr*.pck` | `smaa_output` (the square root of the tone-mapped colour: `gpu/legacy/vk/vk_temporal.cpp:7`) | the frame, R11G11B10F `F`, linear |
| UI and composite | layers `#103`, `#107`; `f_80285b00` | frame + UI layer | window texture `O` |

The network has fixed shares: one program per ratio of `S` to `F` (3/4 for `fd029a317a9a4b94`; "a smaller network, 1/3"
when `S` would exceed `BaseResolution`: the output-size note of the old `nx2/oot_patches.py`, now the At probes in a `PATCH_AT` row in `host/oot_probes.c`; `gpu/legacy/vk/vk_draw.cpp:6380`). It is not a
temporal upscaler: no target carries colour across frames (rt-alloc.md 2: "no TAA history"). On the direct back end it
runs as the game has it, with the matrix multiplies in software (`gpu/direct/direct_compute.cpp:6`, `:80`); it is the
frame's heaviest pass, about 1 ms ([shader-costs.md](shader-costs.md)).

Not established: whether a 1:1 network exists (`S` = `F`, which needs `s` = 4/3 with today's base), and what the game does
with `CreateNdsr` false in `gsys/ModelSceneConfigParam/Main.gsys__ModelSceneConfigParam.bgyml` (the key's name suggests it
gates the network; `CreateFilterAa` and `InitFilterAaSmaaResTextures` would gate SMAA). The guest function that records
the network's dispatches each frame is not identified.

### Motion vectors

**The game writes no per-pixel velocity target.**

- Measured: none of the 60 target names in the census at either output is a velocity or motion target. The
  `Reprojection` target is R11G11B10F (colour) at `O`/4, not a vector format.
- Read: the old renderer's integration states it (`gpu/legacy/vk/vk_temporal.cpp:13-15`: "no velocity target: the scene
  pass draws into one B10G11R11 target, and its upscale network samples the colour alone").
- Inferred: the engine has the means. `f_80021ca0` `model_prev_transform_write` writes previous world matrices
  ("mLastTransform/mPrevTransform"), and `f_8027e840` `layer_post_effects` holds a motion-blur pass. Neither feeds a
  velocity target in the scenes measured. The shader archives were not searched for a velocity output.

What the old renderer derived instead (`gpu/legacy/vk/vk_temporal.cpp:9-24`, `vk_fsr3_inputs.cpp:1-7`):

- the camera: from the draws into the scene's colour target, uniform buffer 3 of the vertex stage (target size at
  0x120, this frame's view-projection at 0x30, projection at 0x70);
- camera motion: depth reprojected through this frame's and the last frame's view-projection (`shaders/dlss_prep.comp`,
  `fsr3_prep.comp`; the math in `host/dlss_math.h`, tested by `tests/test_dlss_math.cpp`);
- skinned motion: the skinned draws drawn again with this frame's and the last frame's bones (`dlss_actor.vert`,
  `dlss_actor.frag`); particles, water, foliage moved in the vertex stage and mesh-shader draws keep camera motion only.

A complete vector field needs the previous transform in the vertex stage of each draw: the previous world matrix the
engine already writes, or the previous bones, and a second colour output in the scene passes.

### What a temporal upscaler needs, and where the game has it

| Input | In the game | Notes |
|---|---|---|
| Jittered projection | Removed (docs/parked-patches.md). `nx2/oot_jitter.py` added a jitter to `P[0][2]`, `P[1][2]` in the view's uniform-buffer fill (`main+0x6bc708`, `0x6bcf8c`, `0x6bcf9c`) when `hos_dlss_jitter_clip` returned one. It asked `rx_option(HOS_OPT_DLSS_JITTER_W/H/PHASES)` (`gpu/api/vk_api.h:23`); the direct back end answers no options (`gpu/direct/direct_main.cpp:393`), so the jitter was always 0 | The sequence is Halton (2, 3) over 8-64 phases (`host/dlss_math.h`) |
| Colour | `color(AA)` R11G11B10F at `S` before SMAA, or `smaa_output` RGB9E5F after it (display-referred, square-root encoded) | Tone mapping happens before SMAA (inferred from the network's input) |
| Depth | `RawDepth(Full)` / `SoftEdgeDepth` DEPTH32F at `S`; 0 at the near plane (not reversed: `gpu/legacy/vk/fsr3_math.h:23`) | FSR's copy is 1 - d |
| Motion vectors | None (above) | Derived as the old renderer did |
| Exposure | Not needed when the upscaler takes the tone-mapped colour; the game's exposure is `f_83485280` `exposure_compute` | |
| Reactive / transparency mask | None | FSR 3.1 can make one from colour (`fsr3_prepare_reactivity`) |
| Hand-back | In place of the network's output (the frame, R11G11B10F `F`, linear), before the UI | The old renderer squared its result back to linear (`dlss_out.comp`) |

### The old renderer's integration (gpu/legacy, not built)

| Piece | Files | State |
|---|---|---|
| Frame chain (camera, inputs pass, actor motion, jitter readback), shared by DLSS and FSR 4 | `gpu/legacy/vk/vk_temporal.cpp`, `vk_temporal.h`, `dlss_actor.h`, `dlss_checks.h`, shaders `dlss_prep.comp`, `dlss_out.comp`, `dlss_actor.*` | Reference |
| AMD FSR 3.1 (FidelityFX SDK v1.1.4's FSR 3.1.4), every pass a dispatch of ours, nothing linked | `vk_fsr3.cpp`, `vk_fsr3_inputs.cpp`, `fsr3_math.h`, 29 compiled `shaders/fsr3_*_spv.h` (upscaler, frame interpolation, optical flow) | SPIR-V headers on disk; the SDK sources (`third_party/fidelityfx-sdk`, `gpu/legacy/scripts/fsr3_shaders.py`) are not |
| AMD FSR 4 through AMD's loader at run time (`amd_fidelityfx_upscaler_vk`) | `vk_fsr4.cpp`, `vk_fsr4.h` (`HOOKSHOT_FSR`) | Needs AMD's library on the PC; on Linux a Vulkan FSR 4 library |
| NVIDIA DLSS through NGX | `vk_dlss.cpp`, `vk_dlss.h`, `vk_ngx_msvc_rt.cpp`; `gpu/legacy/scripts/ngx_sdk.py`, `ngx-sdk.sh` (`HOOKSHOT_NGX`) | Needs the NGX SDK and an NVIDIA GPU |
| Hook point | the network's compute dispatch, recognised by its program key and its images (`vk_draw.cpp:6249-6460`) | |

The build steps went with the old renderers (commit `0bdf4b6`: `nx2/make_game.py` has no DLSS, FSR or shader-seed
steps). `work/deps` holds no FidelityFX or NGX SDK.

Carried over as is: `host/dlss_math.h` (already in the host), `fsr3_math.h`, the FSR 3.1 SPIR-V and the dispatch order of
`vk_fsr3.cpp`. Rewritten: resource binding (the direct back end binds through shader objects and its descriptor heap, not
pipelines and push descriptors), the images (the inputs and FSR's internal targets as back-end images outside the
transient pool, or a reserved range of it), command recording in the back end's lists, and the camera capture (the
direct back end has no per-draw hook that reads uniform buffer 3; the host can hand the camera over instead, from the
same uniform-buffer fill the jitter patches).

On this PC (RX 6950 XT, RADV): FSR 3.1 runs as plain Vulkan compute and is the one to build and test here. FSR 4's
upscaler targets RDNA 4 and is not usable on RDNA 2 (inferred from AMD's requirements, not tried). DLSS needs an NVIDIA GPU
and cannot be tested here; it stays optional.

## Constraints at other sizes

- **The upscale network's shares.** Each network is a fixed ratio of `S` to `F`. A scene size that is not one of the
  shares has no network; the old renderer replaced a missing one with a blit (`vk_draw.cpp:6249-6256`). An arbitrary
  bias therefore needs our upscaler or a plain scale in place of the network.
- **The window textures and `F` are made at boot.** The framework's window textures, the display-size object and the UI
  layer texture are sized once (steps 2-4). Changing `F` in play is not supported by the game; changing `s` is (step 7).
- **The pool.** `hos_rt_pool_size` scales with `O` only. The scene targets grow with `S`'s pixels: at `O` = 3840 x 2160 and `S` = 2880 x 1620
  the transient targets peaked at 884 MB (measured, `work/runs/slots_r2`), so `S` = 3840 x 2160 needs well over 1 GB. The slot range is the
  pool's size again past its end (`host/rt_slots.c` header), so it grows with the pool.
- **Widescreen cap.** `hos_wide_out` is capped at 3840 x 2160's pixels (`host/hos_main.c:1557-1563`); a pool of 1.4 GB
  failed the game's own reservation at boot.
- **UI.** The UI is laid out on a 3840 x 2160 grid (`host/oot_probes.c` `hos_oot_wide_hud`) and kept in a middle 16:9 on
  other shapes. The UI layer is `F`; it scales with `F`, not with `S`.
- **Movies.** `NDVE_Output` is 2560 x 1440 and `MovieRenderer` the movie's size at every output; the movie layer draws
  into the middle 16:9 (`host/oot_probes.c` `hos_oot_wide_movie`).
- **Fixed targets.** The bloom chain (480 x 270 down), the DOF mip chain (960 x 540), the bilateral grid and the occlusion
  depth (256 x 144) do not scale. The occlusion depth feeds the GPU's occlusion test; its size is fixed in the culling
  code's uniforms (inferred).
- **Formats in jobs.** A render job's pass key holds the target formats and size ([job-formats.md](job-formats.md)); a
  format changed at allocation must also change the key the job records with, or the job's pipeline state disagrees with
  its target.
- **Viewports from the CPU.** Passes that size their viewport from the view rectangle (`f_80299510`) and not from the
  target draw into part of a target made larger at allocation; only targets whose pass reads the target's own size
  (`f_802640a0`) can be resized at the allocator alone.

## Control points

### Settings

The host reads `hookshot_options.ini` in the game's folder through `hos_ini_option` (`host/nn_applet.c:92`); its keys
today are `handheld_profile`, `output_res` (`host/nn_applet.c:109`, `:120`) and `frame_rate_cap` (`host/nn_vi.c:119`);
`host/hos_main.c:1533` also reads `ultrawide=`. The Insert menu's options (`rx_option`) have no answers on the direct back
end. The proposal adds:

| Key | Environment | Meaning |
|---|---|---|
| `output_res=window` (or a new key `output_size=WxH`) | `HOS_OUTPUT_RES=window` | `O` = the screen's or the window's size at start, any size (multiples of 8) |
| `render_scale=<percent>` | `HOS_RENDER_SCALE` | the bias: `S` = window size x bias; 100 = the window's size |
| `upscaler=<0 game, 1 fsr3, 2 dlss>` | `HOS_UPSCALER` | who takes `S` to `F` |
| `shadow_res=<px>` | `HOS_SHADOW_RES` | the sun cascades' side; 0 = the game's |
| `local_shadow_scale=<percent>` | `HOS_LOCAL_SHADOW_SCALE` | the local lights' map side, scaled and clamped |

`HOS_RES_SCALE` stays as a raw override of `s`.

### (a) Render resolution = window x bias, with our upscaler

1. **`O` from the window.** Replace the fixed list in `output_mode` / `hos_output_size` (`host/nn_applet.c:116-135`) with
   the screen's size at start (SDL desktop mode, or the window's pixel size), rounded to multiples of 8. Everything in
   steps 2-5 and 9 already follows `hos_output_size`, and `hos_wide_out` / `hos_wide_scene` (`host/hos_main.c:1546-1570`)
   reduce to "`O` has the window's shape": they become one size instead of a correction. The game makes `F` once, so
   "match the screen" means the screen (or the window at start); a later resize changes only the present blit.
2. **`S` from the bias, live.** Set `hos_fixed_scale_bits` so that `S` = window x bias: `s` = bias x (window / `O`) / 0.75
   with today's base (or set edit 3's base to `O` instead of `O * 3/4` and `s` = bias x window / `O`). The fixed path
   recomputes and the game makes new targets the next frame (step 7), so a window resize can follow at run time
   without a scene reload: `realtime_step` (`host/nn_vi.c:230`) already writes the scale every present. Lift the 2.0 cap
   in `hos_oot_scale_recompute` / `hos_oot_scene_mode` (`host/oot_probes.c`) if a bias above it is wanted.
3. **Bypass the network.** Patch the game where it records the network's dispatch (not yet identified: find it from
   `f_81503ce0` `ndsr_networks_init`'s object and the dispatch with program `fd029a317a9a4b94`) so that it calls a host
   function instead, which asks the back end for an upscale: a new `MkwVkRenderer` entry such as
   `upscale(in_colour, depth, camera, jitter, out)` in `gpu/api/vk_api.h` (bumps `MKWVK_API_VERSION`). The game decides
   when (its own call site); the back end runs a plain operation on images it is handed. Inputs: `color(AA)` (pre-SMAA)
   or `smaa_output`, `RawDepth(Full)`, the camera from the uniform-buffer fill that the parked camera jitter patched (docs/parked-patches.md). Output:
   the network's output image at `F`. Skip SMAA with it (patch `f_80270da0` to copy) when the upscaler does its own AA.
   Also check `CreateNdsr` in a romfs overlay first: if the game then renders `S` straight into the frame with a plain
   scale, that is the data-only bypass.
4. **Jitter.** Make `hos_dlss_jitter_clip` (`host/rx_backend.c:331`) read the host's own state (render size, phases)
   instead of `rx_option`, so the camera jitter's sites (parked: docs/parked-patches.md) act on the direct back end.
5. **Pool.** Size `hos_rt_pool_size` from `max(S_max, F)` pixels, not `O` alone, with `S_max` = window x the largest bias
   allowed.
6. **Present.** No change: the blit takes the window texture (`O`) to the swapchain. With `O` = screen size the blit is 1:1
   in fullscreen.

Risk: medium. The network's dispatch site is unknown, and the upscaler's inputs (motion without a velocity target) limit
quality on animated geometry. Size: FSR 3.1 in the direct back end about 1.5-2k lines (passes, images, dispatches,
inputs pass), the host and patch side a few hundred.

### (b) Configurable shadow resolution

- **Sun cascades.** Data first: a romfs overlay of `gsys/ModelSceneConfigParam/Main.gsys__ModelSceneConfigParam.bgyml`
  with `DepthShadowTexWidth`/`Height` (and the cascade count and splits). If the overlay is not general enough, an
  `ENTRY_HOOK(0x80669460)` (`pass_sun_shadow_alloc`) that writes `+0x70` / `+0x90` of its object once. Check that the
  cascades' viewport and the shaders' texel size come from the target, and scale `DepthShadowPcfOffset` with the side.
- **Local lights.** `alloc_pre` already rewrites the descriptor for `LR_LOCAL_SHADOW` (`host/rt_slots.c:493`); multiply the
  side there by `local_shadow_scale` and clamp (4096). The pass reads the target's width for its viewport, so nothing
  else changes.
- **Static shadow.** Leave it: 302 MB at 12288^2 is already the largest image.

Risk: low for local lights, medium for the cascades (fit and filter in texels). Size: small (tens of lines plus an
overlay file built on the PC).

### (c) The host decides any target's size and format

Insertion point: `alloc_pre` in `host/rt_slots.c:476`, the entry of `f_8005ed00` `dta_alloc`. Every target, transient
and persistent, passes through it with its name, caller and descriptor, before the game computes storage. A policy table
keyed by (caller function, name) would rewrite `+0x8` format, `+0x10` width, `+0x14` height, `+0x1c` levels. Why there: one
place, in the game's own allocator, with names; the back end keeps creating what NVN describes.

The table is safe only for targets whose pass reads the target's size (local shadows; inferred for the bloom, DOF and
bilateral chains, to be checked per pass). Scene-sized targets follow the view rectangle and change through `s` (a), not
per target. A format change must also reach the job's pass key ([job-formats.md](job-formats.md)) and must be one the
shaders write and sample the same way (R11G11B10F to RGBA16F is; a depth format to another is not without the
shaders' compare state).

### Order

1. Census at the new sizes: one `HOS_RT_SLOTS_LOG=1` run each at `O` = 2560 x 1440 and at an odd window size with
   dynamic resolution off, to confirm the derivations above.
2. (a) step 1 and 5: `O` from the screen, pool from `S_max`; game's network kept (bias limited to its shares).
3. (b) local lights in `alloc_pre`; then the cascades by overlay.
4. Find the network's dispatch site; try `CreateNdsr` off by overlay.
5. (a) steps 3-4: the upscale entry in `vk_api.h`, FSR 3.1 ported into `gpu/direct`, the jitter on; then `render_scale`
   free.
6. Motion: the depth reprojection first; skinned and per-object motion from the previous transforms later.
7. (c) the policy table, target by target, each checked in the game.

## Not established

- The values of the shadow keys in the scene config (names only), and which code reads them into `+0x70` / `+0x90`.
- The guest function that records the upscale network's dispatches; whether a 1:1 network exists; what `CreateNdsr`
  false does.
- Whether tone mapping is applied before `color(AA)` (inferred from the network's input).
- The persistent targets' exact sizes and formats at other outputs (the census logs transient targets only).
- Velocity outputs in the shader archives (not searched).
- Title screen and pause-menu targets beyond the UI captures in the census.
