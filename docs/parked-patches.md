# Parked patches: heart protection, flight, free camera (removed)

This page records four game patches and the host code they drove. They were removed from the build when the present
lifter (the lifter that wrote C, whose text these patches edited) was retired. Nothing else of them is left in the
repository. They were cheats and debug tools, all off by default.

To bring one back, write it as a probe (a `PATCH_AT` declaration, `host/patch.h`, docs/lifter.md "Hooks and
probes"): declared registers, a host function of plain values and typed structs, and the instruction word checked at the
build.

## What went with them

The flight hook drove more than flight. Every frame the player was updated, `player_movement` called the host
(`hos_oot_lift_player`), and that one call ran all of the following:
- the Cheats page of the Insert menu: inventory and pouch edits, setting the time, teleport to any map, the age toggle;
- the test runs' live commands `!cheat <n>` and `!warp <banc> [startpos] [boot event]` (`HOS_INPUT_LIVE`, used by
  `scripts/direct_scene_cycle.sh` and the zoo's `live.txt`);
- the `scene: ready <banc>` log line;
- `HOS_BENCH`'s "in play" signal (`hos_bench_player`): an At probe of its own now (`hos_oot_player_update`).

The others stopped working when the patches were parked, and the direct back end has no Insert menu.

Game build: OoT 1.1.1, main build id e0f3ccfaf50065ae4ea01eb5aeb13b1a45da3348. The labels are in `games/oot/labels.txt`.

## The patches

| patch | function (label) | site | what the player got | switch (default) |
|---|---|---|---|---|
| heart protection | `f_804bd8c0` (`actor_life_write`) | before the two `cas w11, w12, [x13]` at `0x804bdcf8` and `0x804bded0` (w11 the old life, w12 the new, x13 its address, x19 the actor) | a hit does not lower Link's life | Cheats page, `HOS_OOT_PROTECT_HEARTS=1` (off) |
| flight | `f_82c415e0` (`player_movement`) | before `tbnz w8, #1` at `0x82c4172c` (x19 the player actor); after `0x82c41778` (x25 the pose matrix) | Link flies: the stick moves him in the view's plane, B or R up, L down (Page Up/Down on the keyboard), a double B toggles flight; the Cheats page's actions ran here too | Cheats page (off) |
| flight rotation | `f_806d2530` (`phys_cct_change_position`, vtable `0x84c54f10` +0xe8) | before `blr x8` at `0x806d25c4` (x0 the controller, x1 the matrix) | the flying Link turns toward his motion | as flight |
| free camera | `f_804e48c0` (`camera_view_loop`) | the view loop's head, `0x804e5068` (x20 the view) | a free-flying camera: the sticks move and turn it, L and R change height | Cheats page (off) |

### Guest objects (struct sketches, offsets as used)

```c
typedef struct {            /* the player actor (f_82c415e0's x19), vtable 0x84b45e18 */
    uint64_t vtable;        /* +0 */
    uint8_t  _0[616];
    uint64_t components;    /* +624: its +800 is the movement; the life-check actor is movement - 0x1d50,
                               movement + 24 points back to this actor */
    uint8_t  _1[796 - 632];
    float    position[3];   /* +796 */
    uint8_t  _2[816 - 808];
    float    forward_x;     /* +816 (yaw = atan2(+816, +840)) */
    uint8_t  _3[840 - 820];
    float    forward_z;     /* +840 */
    uint8_t  _4[7184 - 844];
    uint64_t life_record;   /* +7184: the life value is at life_record + 8 */
} PlayerActor;

typedef struct {            /* the actor whose life is written (vtable 0x84b6c460); other actors share the vtable */
    uint64_t vtable;        /* +0 */
    uint8_t  _0[8];
    uint64_t resource;      /* +16: the record's +88 holds "Player\0" (u32 0x79616c50, u16 0x7265, u8 0) for Link */
} ActorWithResource;

typedef struct {            /* the gameplay view (vtable 0x84c18a88): camera 0's */
    uint64_t vtable;        /* +0 */
    float    view[3][4];    /* +8: the view matrix (rows right, up, back; w = -axis . eye) */
    uint8_t  _0[96 - 56];
    double   eye[3];        /* +96 */
    double   target[3];     /* +120 */
} GameplayView;

/* The pose and rotation matrices (x25 in f_82c415e0, x1 in f_806d2530) are rows of 4 floats, 16 bytes apart; the
   3x3 rotation at +0, +16, +32 was overwritten with a rotation about y alone. */
```

### Guest calls the host made

Each call saved the caller's whole context, set the registers and guest stack, called the lifted function, then
restored the context.

| function (label) | arguments | what the call achieves |
|---|---|---|
| `f_806d2530` (`phys_cct_change_position`) | x0: Link's physics controller (from the components through +80, +32, +16, +8; vtable `0x84c54f10`, its +0xe8 = `0x806d2530` checked); x1: `{x, y, z, 0}` on the guest stack at `sp - 16` | moves Link to the flight's target with the game's own collision bookkeeping |
| `f_828d4a70` (`ui_pouch_add_remove`, OneShotAddOrRemovePouchContent) | x0: an event block on the guest stack at `sp - 0xe0`: the actor name at +0x80; pointers +0x20 → +0x60, +0x30 → +0x70, +0x38 → +0x68, +0x48 and +0x50 → +0x74, +0x60 → the name; +0x68 = `0x83f0a6d8` ("Script"); +0x70 = delta | adds or removes a pouch content by name; the status is the result word's top 2 bits (+0x18 >> 30) |
| `f_82a3c260` / `f_805b63e0` (`ui_pouch_count_query`, `ui_pouch_count`) | the pouch manager, `&name`, 0, 0 | counts one content |
| `f_8022d9c0` / `f_80cd0ea0` (`gamedata_get_bool`, `gamedata_get_string`) | GameDataMgr, a MurMur3 name hash | reads `Sequence_IsLoadingBanc` (`0xef2b60c2`), `Sequence_IsChildhood` (`0xcea220d3`), `Sequence_CurrentBanc` (`0x1d6189da`) |
| `f_834722d0` (ExecuteWmSetTime) | the world manager (`[[0x84c9e898]]`, its +0x510 must be set), seconds since midnight | sets the time of day |
| `f_80d99f10`, `f_802dc440`, `f_80d9a820`, `f_8022b6f0` (`event_make_arg`, `event_set_link`, `event_request`, `event_release_link`) | an EventRequestArg of `0x1b20` bytes on the guest stack; the EventMgr (`[0x84c9e090]`) | requests a map change through the event `Dm_SY_BancChangeGo` |

Singletons used: `0x84c9e0d0` (pouch), `0x84c9e2a8` (state), `0x84eb6888` (actor database), `0x84e8c060` (actor
info), `0x84c9e090` (EventMgr), `0x84c9e898` (world manager).

### The logic, as pseudo-code

```
heart protection, before each CAS of the life value:
    if new < old and the cheat is on and actor (x19) is Link (vtable, resource "Player")
       and x13 == actor.life_record + 8:  new = old            # the write keeps the old life

flight, before the movement's tbnz (once a frame per player update):
    if actor is the player (vtable 0x84b45e18, components.movement points back, its actor is Link):
        under a try-lock (Link is updated by different workers on successive frames):
            run the Cheats page action (or a live "!cheat"); check a pending warp or teleport
            if flying: target += velocity * dt, where velocity eases (1 - e^(-12 dt)) toward the stick's direction
                       in the view's plane at 4 m/s; yaw turns toward the motion at up to 8 rad/s
                       call phys_cct_change_position(controller, &target)   # the game's own move
                       then on the same frame write the yaw-only rotation into the pose matrix
                       (after 0x82c41778) and into the controller's matrix (before f_806d2530's blr);
                       without the rotation writes, Link flew facing his last walking direction
            a target that went stale (no update for 250 ms, another actor) restarts from Link's position
    action 2 marks the position, action 3 returns to the mark (by the same move call)

free camera, at the view loop's head:
    if the view is the gameplay view (camera 0's) and the cheat is on:
        on the first frame take eye and yaw/pitch from the view's eye and target (as doubles)
        sticks: yaw and pitch at 1.6 and 1.2 rad/s; move at 8 m/s (Ctrl: 30) in the view's axes; L/R or PgUp/PgDn: height
        write the 3x4 view matrix at view + 8; the game draws from it

teleport and age (Cheats page, live "!warp"):
    refuse while an event plays (EventMgr+104), a map loads, or a request is less than 8 s old
    make_arg(arg on the guest stack); set the event name and its arguments NextBanc, NextStartPos, BootEventName
    (strings in host-allocated guest memory that outlives the call) and IsCheckAir; set_link(arg + 0x1950, Link)
    as the starter; request(EventMgr, arg); release both links (arg + 0x1950 and + 0x1968, first set to 0x84994b20)
    age: flip Sequence_IsChildhood in GameDataMgr's bool table (+24 entries of 24 bytes, value at +22; +32 the hash
    table), then teleport to the current map's other-age destination (the Market if none); undo the flip if no request
```

### What a probe-style re-implementation would need

- **Heart protection:** two `At(before=True)` rows at the CAS instructions,
  `w12 := hos_life_guard(w11, w12, x13, x19)`, with the actor checked through `PlayerActor` and `ActorWithResource`.
  This fits the style.
- **Free camera:** an At row at the loop head (x20) and a typed `GameplayView` write. Its input reading is host code.
  This fits the style.
- **Flight and its rotation:**
  - The pose and rotation writes are an At row each, with plain registers and a typed matrix.
  - The move itself calls the game's `phys_cct_change_position` with an argument on the guest stack. That needs a way
    to call guest code from a host function without the caller's context, and none exists.
- **The Cheats page's actions** (pouch, time, teleport, age) need the same kind of guest calls.
- **The live commands and `scene: ready`** were test tooling built on those actions.

# Parked patch: the temporal upscalers' camera jitter (removed)

`nx2/oot_jitter.py` added a sub-pixel jitter to the scene's projection for a temporal upscaler (NVIDIA DLSS, AMD FSR 3.1
and 4, `gpu/legacy/vk`). It was C after three instructions on the whole guest context. Nothing could reach it on the
direct back end: the jitter came from `rx_option(HOS_OPT_DLSS_JITTER_W/_H)`, which the direct back end does not answer,
so it was always 0. It was removed with the last whole-context sites (declared rows only, docs/lifter.md "Hooks and
probes"). `host/dlss_math.h` (the Halton sequence, the clip-space offset and the projection check) stays, with
`tests/test_dlss_math.py`.

| function | site | registers |
|---|---|---|
| `f_806dc520` (main+0x6bc520: fills each view's uniform buffers) | after `ldp q21, q22, [x8, #0x20]` at main+0x6bc708 (word `ad415915`) | rows 0 and 1 in q19, q20 (loaded just before) |
| | after `ldr q26, [x9]` at main+0x6bcf8c (`3dc0013a`) | row 0 in q26 |
| | after `ldr q6, [x9]` at main+0x6bcf9c (`3dc00126`) | row 1 in q6 |

The view is x28 at all three; its projection's rows are at x28 + 0x1a8, 16 bytes apart. The jitter goes on the third
column of the first two rows (P[0][2], P[1][2]) in the registers only, so the uniform buffers get the jittered matrix and
everything made from it, while the game's cameras (and the shadow cascades fitted to them on the CPU, #841) keep the
projection as it is.

```
at each site, with x the row register of P[0] (or none) and y that of P[1] (or none):
    j = jitter_clip(projection = x28 + 0x1a8)      # 0 unless an upscaler runs and the projection is the picture's:
                                                   # perspective, of 16:9 or the window's ratio (dlss_jitter_clip)
    if j: x.lane[2] += j.x; y.lane[2] += j.y       # (float lanes; the frame's Halton (2, 3) offset in clip space,
                                                   #  over dlss_jitter_phases(render, output) frames)
```

As a row it would be `At(0x806dc520, pc, word, "jitter", ("x28", "q19", "q20"), io=("q19", "q20"))` (and one io
register for the other two sites).
