# Parked patches

This page records features that were removed from the code, with enough detail to rebuild one. Read it before you
bring back a cheat, a debug tool, an experiment, or another game's patch, so you start from what is known.

Each section says what the feature did, why it was parked, and what you need to rebuild it. Addresses are for the game
build that `games/oot/game.py` names (main build ID `e0f3ccfaf50065ae4ea01eb5aeb13b1a45da3348`); the labels are in
`games/oot/labels.txt`. Rebuild a feature as a patch declaration ([Write a patch](patches.md)): declared registers, a
host function of plain values and typed structs, and the instruction word checked at the build.

## Heart protection, flight and free camera

**What they did.** Three cheats from an Insert-menu Cheats page, all off by default:

| Patch | Function (label) | Site | Effect |
|---|---|---|---|
| Heart protection | `f_804bd8c0` (`actor_life_write`) | Before the two `cas w11, w12, [x13]` at `0x804bdcf8` and `0x804bded0`: `w11` the old life, `w12` the new, `x13` its address, `x19` the actor | A hit does not lower Link's life |
| Flight | `f_82c415e0` (`player_movement`) | Before `tbnz w8, #1` at `0x82c4172c` (`x19` the player actor); after `0x82c41778` (`x25` the pose matrix) | The stick moves Link in the view's plane; B or R up, L down; a double B toggles flight |
| Flight rotation | `f_806d2530` (`phys_cct_change_position`, vtable `0x84c54f10` +0xe8) | Before `blr x8` at `0x806d25c4` (`x0` the controller, `x1` the matrix) | The flying Link turns toward his motion |
| Free camera | `f_804e48c0` (`camera_view_loop`) | The view loop's head, `0x804e5068` (`x20` the view) | The sticks move and turn the camera; L and R change height |

The flight hook also drove the Cheats page's actions (pouch edits, time of day, teleport, the age toggle), the test
runs' live commands `!cheat <n>` and `!warp <banc> [startpos] [boot event]`, and the `scene: ready <banc>` log line.

**Why parked.** They edited the C text of a lifter that no longer exists, and the direct back end has no Insert menu.

**Guest objects** (offsets as used):

```c
typedef struct {            /* the player actor (f_82c415e0's x19), vtable 0x84b45e18 */
    uint64_t vtable;        /* +0 */
    uint8_t  _0[616];
    uint64_t components;    /* +624: its +800 is the movement; movement + 24 points back to this actor */
    uint8_t  _1[796 - 632];
    float    position[3];   /* +796 */
    uint8_t  _2[816 - 808];
    float    forward_x;     /* +816 (yaw = atan2(+816, +840)) */
    uint8_t  _3[840 - 820];
    float    forward_z;     /* +840 */
    uint8_t  _4[7184 - 844];
    uint64_t life_record;   /* +7184: the life value is at life_record + 8 */
} PlayerActor;

typedef struct {            /* an actor whose life is written (vtable 0x84b6c460, shared with other actors) */
    uint64_t vtable;
    uint8_t  _0[8];
    uint64_t resource;      /* +16: the record's +88 holds "Player\0" for Link */
} ActorWithResource;

typedef struct {            /* the gameplay view (vtable 0x84c18a88), camera 0's */
    uint64_t vtable;
    float    view[3][4];    /* +8: rows right, up, back; w = -axis . eye */
    uint8_t  _0[96 - 56];
    double   eye[3];        /* +96 */
    double   target[3];     /* +120 */
} GameplayView;
/* The pose and rotation matrices are rows of 4 floats, 16 bytes apart; the 3x3 rotation at +0, +16, +32 was
   overwritten with a rotation about y alone. */
```

**Guest functions the host called:**

| Function (label) | Arguments | Purpose |
|---|---|---|
| `f_806d2530` (`phys_cct_change_position`) | `x0` Link's physics controller (from the components through +80, +32, +16, +8); `x1` `{x, y, z, 0}` on the guest stack | Moves Link with the game's own collision bookkeeping |
| `f_828d4a70` (`ui_pouch_add_remove`) | `x0` an event block on the guest stack (actor name at +0x80; +0x68 = `0x83f0a6d8`, "Script"; +0x70 the delta) | Adds or removes a pouch content by name |
| `f_82a3c260`, `f_805b63e0` (`ui_pouch_count_query`, `ui_pouch_count`) | The pouch manager, `&name`, 0, 0 | Counts one content |
| `f_8022d9c0`, `f_80cd0ea0` (`gamedata_get_bool`, `gamedata_get_string`) | GameDataMgr and a MurMur3 name hash | Reads `Sequence_IsLoadingBanc` (`0xef2b60c2`), `Sequence_IsChildhood` (`0xcea220d3`), `Sequence_CurrentBanc` (`0x1d6189da`) |
| `f_834722d0` (`world_set_time`) | The world manager (`[[0x84c9e898]]`), seconds since midnight | Sets the time of day |
| `f_80d99f10`, `f_802dc440`, `f_80d9a820`, `f_8022b6f0` (`event_make_arg`, `event_set_link`, `event_request`, `event_release_link`) | An EventRequestArg of `0x1b20` bytes on the guest stack; the EventMgr (`[0x84c9e090]`) | Requests a map change through the event `Dm_SY_BancChangeGo` |

Singletons: `0x84c9e0d0` (pouch), `0x84c9e2a8` (state), `0x84eb6888` (actor database), `0x84e8c060` (actor info),
`0x84c9e090` (EventMgr), `0x84c9e898` (world manager).

**The logic:**

```
heart protection, before each CAS of the life value:
    if new < old and the actor (x19) is Link (vtable, resource "Player") and x13 == life_record + 8: new = old

flight, before the movement's tbnz, under a try-lock (Link is updated by different workers on successive frames):
    target += velocity * dt; velocity eases (1 - e^(-12 dt)) toward the stick's direction in the view's plane at 4 m/s
    yaw turns toward the motion at up to 8 rad/s
    call phys_cct_change_position(controller, &target), then write the yaw-only rotation into the pose matrix
    (after 0x82c41778) and the controller's matrix (before f_806d2530's blr); without the rotation writes, Link
    flew facing his last walking direction. A target with no update for 250 ms restarts from Link's position.

free camera, at the view loop's head, for camera 0's view:
    first frame: eye and yaw/pitch from the view's eye and target
    sticks: yaw 1.6 and pitch 1.2 rad/s; move 8 m/s (Ctrl: 30) in the view's axes; write the 3x4 matrix at view + 8

teleport and age:
    refuse while an event plays (EventMgr+104), a map loads, or the last request is under 8 s old
    make_arg on the guest stack; set NextBanc, NextStartPos, BootEventName (strings in guest memory that outlives the
    call) and IsCheckAir; set_link(arg + 0x1950, Link); request(EventMgr, arg); release both links (arg + 0x1950 and
    + 0x1968, first set to 0x84994b20)
    age: flip Sequence_IsChildhood in GameDataMgr's bool table, then teleport to the map's other-age destination
```

**To rebuild.** Heart protection is two `before` rows at the CAS instructions that return `w12`. The free camera is a
row at the loop head with a typed `GameplayView` write. Flight is rows for the pose and rotation writes plus a host
function that calls `phys_cct_change_position` through `guest(...)` and `guest_call`, with its argument on the guest
stack; the Cheats page's actions need the same kind of guest calls. `scripts/direct_scene_cycle.sh` still sends the
live commands.

## Temporal upscalers' camera jitter

**What it did.** It added a sub-pixel jitter to the scene's projection for a temporal upscaler (DLSS, FSR) by editing
the projection rows in registers, so the uniform buffers got the jittered matrix while the game's cameras and shadow
cascades kept the original.

| Function | Site | Registers |
|---|---|---|
| `f_806dc520` (`ubo_view_camera_write`) | After `ldp q21, q22, [x8, #0x20]` at `0x806dc708` (word `ad415915`) | Rows 0 and 1 in `q19`, `q20` |
| | After `ldr q26, [x9]` at `0x806dcf8c` (word `3dc0013a`) | Row 0 in `q26` |
| | After `ldr q6, [x9]` at `0x806dcf9c` (word `3dc00126`) | Row 1 in `q6` |

The view is `x28` at all three sites; its projection's rows are at `x28 + 0x1a8`. The jitter goes on the third column
of the first two rows (P[0][2], P[1][2]): 0 unless an upscaler runs and the projection is the picture's (perspective,
16:9 or the window's ratio); otherwise the frame's Halton (2, 3) offset in clip space.

**Why parked.** The direct back end runs no temporal upscaler, so the jitter was always 0.

**To rebuild.** An `after` row at each site with `x28` as an operand and the row registers as `io(...)`.
`host/dlss_math.h` (the Halton sequence, the clip-space offset and the projection check) and
`tests/test_dlss_math.py` remain.

## Mario Kart World's frame pacer

**What it did.** Behind a run-time title check (main's build ID against MKW's, `d00e4f399e411d0613aa8d26b1c02fef`),
`nvnWindowSetPresentInterval` kept the caller's `x19`, MKW's pacer object, when its return address was `0x806bce28`.
Every present then wrote `HOS_MKW_INTERVAL` (default 1) into the pacer's override at `+0x210`: 1 gave a steady 60 fps,
2 gave 30, and 0 left the game's choice, which swung between the two with stutters. MKW's timing counts vsyncs, so an
interval the renderer cannot keep slows the game rather than dropping frames. A log line every 5 s showed the pacer's
frame work (`+0x128`), running excess (`+0x218`) and interval (`+0x214`).

**Why parked.** The host serves only the game its description names.

**To rebuild.** A `games/mkw` description and rows with MKW's words at the pacer's call, instead of a check of the
return address.

## Mario Kart World's other host special cases

- **Idle thread dump.** `HOS_STATS_AFTER`'s thread dump skipped threads whose last return address was `0x825c6844`,
  where MKW's idle threads wait.
- **Occlusion grid reader.** The hook on OoT's `agl::utl::DepthBufferEx` reader (`0x80365f00`) declined at once for
  MKW, which has other code there.

Both were parked for the same reason as the pacer and would come back the same way.

## TOTK's stand-ins

**What they did.** Host functions that Tears of the Kingdom's runtime called, each a no-op or identity for OoT: a cached
fused-weapon icon answered "not found" (`hos_blank_cached_icon`), ultrawide HUD files redirected to other paths
(`hos_ultrawide_redirect`, `hos_ultrawide_apply`, `hos_view_aspect`), a filter of the title menu's buttons
(`hos_title_input`), the profiler's A/B phases (`hos_ab`), and a memory map report (`HOS_MEM_MAP=1`).

**Why parked.** OoT never needed them.

**To rebuild.** Patches in a TOTK game description.

## SDK functions the image does not contain

**What they did.** Host replacements for `nn::os::SystemEvent`'s constructor and destructor (the wrapper holds a
`SystemEventType` at offset 0) and for the older SDK's `nn::audio::SetVoiceBiquadFilterParameter` with Q14 coefficients
(bool enable at 0, int16 numerator[3] at 2, int16 denominator[2] at 8, each divided by 16384).

**Why parked.** No module of OoT's image has these symbols, and the lift now stops on such a declaration.

**To rebuild.** An `HLE` in the host when a game's image contains the symbol.

## Present without waiting

**What it did.** `HOS_PRESENT_NOWAIT=1` made `nvnEventGetValue` answer "signaled" at once when called from OoT's
present poll in `f_805902c0` (`frame_present_submit`, return addresses `0x805904a0` and `0x80590530`; the loop
`while (nvnEventGetValue(e) != 1) SleepThread(...)` at `0x80590500`). The game could then go on to the next frame
while this one was replayed. No measurement was recorded.

**Why parked.** Behavior keyed on a return address is not kept.

**To rebuild.** An `instead` row at the poll's call of `nvnEventGetValue`.

## Shorter guest sleeps

**What it did.** `HOS_POLL_SLEEP_NS=<ns>` shortened every guest sleep of at most 100 us to `<ns>`, so a job system that
sleeps 20 us between looks notices the end sooner. Measured on TOTK: no change.

**Why parked.** No measured gain.

**To rebuild.** A row at the sleeping call that should wake sooner, as `hos_oot_calc_sleep` does for the calc runner.
