# Parked patches: heart protection, flight, free camera (removed)

The record of four game patches and the host code they drove, removed from the build once the present lifter (the one
that wrote C, whose text these patches edited) was retired. Nothing of them is left in the repository but this page.
They were cheats and debug tools, off by default. Bringing one back means a lift 2 probe (`nx2/lift2/probes.py`,
docs/lifter2-design.md sections 14i and 14j): declared registers, a host function of plain values and typed structs,
the instruction word checked at lift time.

What went with them, because only the flight hook drove it (every frame the player was updated,
`hos_oot_lift_player` called `oot_update_player_cheats`): the whole Cheats page of the Insert menu (inventory and
pouch edits, set time, teleport to any map, the age toggle), the test runs' live commands `!cheat <n>` and
`!warp <banc> [startpos] [boot event]` (`HOS_INPUT_LIVE`, used by `scripts/direct_scene_cycle.sh` and the zoo's
`live.txt`), the `scene: ready <banc>` log line, and `HOS_BENCH`'s "in play" signal (`hos_bench_player`). All of these
were dormant since the patches were parked; the direct back end has no Insert menu.

Game build: OoT 1.1.1, main build id e0f3ccfaf50065ae4ea01eb5aeb13b1a45da3348. Labels: `nx2/oot_labels.txt`.

## The patches

| patch | function (label) | site | what the player got | switch (default) |
|---|---|---|---|---|
| heart protection | `f_804bd8c0` (`actor_life_write`) | before the two `cas w11, w12, [x13]` at `0x804bdcf8` and `0x804bded0` | a hit does not lower Link's life | Cheats page, `HOS_OOT_PROTECT_HEARTS=1` (off) |
| flight | `f_82c415e0` (`player_movement`) | before `tbnz w8, #1` at `0x82c4172c` (x19 the player actor); after `0x82c41778`'s label (x25 the pose matrix) | Link flies: the stick moves him in the view's plane, B/R up, L down (Page Up/Down on the keyboard), double B toggles; the Cheats page's actions run here too | Cheats page (off) |
| flight rotation | `f_806d2530` (`phys_cct_change_position`, vtable `0x84c54f10` +0xe8) | before `blr x8` at `0x806d25c4` (x0 the controller, x1 the matrix) | the flying Link turns toward his motion | as flight |
| free camera | `f_804e48c0` (`camera_view_loop`) | the view loop's head, `0x804e5068` (x20 the view) | a free-flying camera: sticks move and turn it, L/R height | Cheats page (off) |

### Guest objects (struct sketches, offsets as used)

```c
typedef struct {            /* the player actor (f_82c415e0's x19), vtable 0x84b45e18 */
    uint64_t vtable;        /* +0 */
    uint8_t  _0[616];
    uint64_t components;    /* +624: components; +800 of it the movement (actor at movement - 0x1d50, movement+24 back to the actor) */
    uint8_t  _1[796 - 632];
    float    position[3];   /* +796 */
    uint8_t  _2[816 - 808];
    float    forward_x;     /* +816 (yaw = atan2(+816, +840)) */
    uint8_t  _3[840 - 820];
    float    forward_z;     /* +840 */
    uint8_t  _4[7184 - 844];
    uint64_t life_record;   /* +7184: the life value is at life_record + 8 */
} PlayerActor;

typedef struct {            /* the actor hos_oot_is_player checks (vtable 0x84b6c460): Link's resource record */
    uint64_t vtable;        /* +0 */
    uint8_t  _0[8];
    uint64_t resource;      /* +16: its +88 is "Player" (u32 0x79616c50, u16 0x7265, u8 0) */
} ActorWithResource;

typedef struct {            /* the gameplay view (vtable 0x84c18a88; camera 0's, oot_gameplay_view) */
    uint64_t vtable;        /* +0 */
    float    view[3][4];    /* +8: the view matrix the free camera writes (rows: right, up, back; w = -axis . eye) */
    uint8_t  _0[96 - 56];
    double   eye[3];        /* +96 */
    double   target[3];     /* +120 */
} GameplayView;

/* the pose / rotation matrices (x25 in f_82c415e0, x1 in f_806d2530): rows of 4 floats, 16 bytes apart; the
   3x3 rotation at +0, +16, +32 was overwritten with a yaw-only rotation */
```

### Guest calls the host made (with the caller's context: saved, changed, restored)

- `f_806d2530` `phys_cct_change_position(controller, &target)`: the flight's move. The target (3 floats and 0) on the
  guest stack at `sp - 16`; the controller reached from the player's components through +80, +32, +16, +8, its
  vtable checked (`0x84c54f10`, +0xe8 = `0x806d2530`).
- `f_828d4a70` OneShotAddOrRemovePouchContent (the event parameters built on the guest stack at `sp - 0xe0`, with the
  string "Script" `0x83f0a6d8`), `f_82a3c260` / `f_805b63e0` pouch counts, `f_8022d9c0` / `f_80cd0ea0` GameDataMgr
  bool and String64 getters, the event request for a map change (EventRequestArg entries of 200 bytes), singletons at
  `0x84c9e0d0` (pouch), `0x84c9e2a8` (state), `0x84eb6888` (actor database), `0x84e8c060` (actor info), `0x84c9e090`
  (EventMgr). The teleport destinations were a generated table (`work/oot-teleport-tools/write_destinations.py`).

### What a probe-style re-implementation would need

- Heart protection: `Before` rows at the two CAS (w12 := `hos_life_guard(w11, w12, x13, x19)`), the host function
  checking the actor through typed structs (`PlayerActor`, `ActorWithResource`). Fits the style.
- Flight and its rotation: the movement and pose are a `Before` / `After` row each with plain registers, but the move
  itself calls the game's `phys_cct_change_position` with an argument on the guest stack: that needs a context-free
  way to call guest code from a host function (none exists; the old code saved and restored the caller's whole Ctx).
- Free camera: an `After` row at the loop head (x20) and a typed `GameplayView` write; its input reading is host code.
  Fits the style once the view is a struct.
- The Cheats page actions (inventory, teleport, time, age) need guest calls like flight's; the live commands and
  `scene: ready` were test tooling on top of them; `HOS_BENCH`'s "in play" needs a player-update probe of its own.

## The code as it was (ours, not the game's)

### The text patches of the present lifter's C (`nx2/oot_parked.py`)

```python
"""Parked patches of the lifted C for Ocarina of Time, not applied while the present lifter is being retired.

Each was a text edit of the present lifter's C (nx2/oot_patches.py applied them); lift 2 writes no C, so they wait here
until each is ported to a guest-address probe (nx2/lift2/probes.py) or dropped. (Ported: the native mouse camera and
text speed, At probes.) Nothing calls this module: the lift and
the build leave these functions to lift 2 unchanged, and the host's switches for them do nothing meanwhile. The table of
what each did, its switch and what is lost while parked: docs/lifter2-design.md, "Parked patches". The text is kept as
it was, as the reference for the port.

- Player heart protection (patch_life, f_804bd8c0), player flight (patch_movement, f_82c415e0) and its rotation
  (patch_flight_rotation, f_806d2530): host/oot_cheats.c's switches.
- Free camera (patch_view, f_804e48c0's view loop): hos_oot_free_view.
"""

LIFE_HEAD = "void f_804bd8c0(Ctx *__restrict A64_NOESC c) {\n"
MOVE_HEAD = "void f_82c415e0(Ctx *__restrict A64_NOESC c) {\n"
MOVE_ANCHOR = ("if (((c->x[8] >> 1) & 1)) { goto L_82c41748; }\n")
MOVE_MATRIX = "L_82c41778:;\n"
WARP_HEAD = "void f_806d2530(Ctx *__restrict A64_NOESC c) {\n"
WARP_ANCHOR = "{ uint64_t _t = c->x[8]; c->x[30] = 0x806d25c8ULL; A64_ICALL(c, _t); }\n"
VIEW_HEAD = "void f_804e48c0(Ctx *__restrict A64_NOESC c) {\n"
# (the head of the view's loop: a bare label, since nx2/poll.py puts the interrupt poll before each backward goto; with the
# poll after the label in C lifted before that)
VIEW_ANCHORS = ("L_804e5068:;\n", "L_804e5068:; A64_POLL(c);\n")
LIFE_CAS = ("{ uint64_t _a = c->x[13]; uint32_t _e = (uint32_t)c->x[11]; "
            "__atomic_compare_exchange_n((uint32_t *)A64_PTR(_a), &_e, (uint32_t)c->x[12], "
            "0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); A64_MARK(_a, 4); c->x[11] = (uint64_t)_e; }")


def patch_life(source):
    start = source.find(LIFE_HEAD)
    if start < 0:
        return None
    end = source.find("\n}", start)
    if end < 0 or source[start:end].count(LIFE_CAS) != 2:
        return None
    # x19 is the actor. Confirm its type and life pointer before protecting this write.
    guard = ("    if ((uint32_t)c->x[12] < (uint32_t)c->x[11] && hos_oot_hearts_enabled() &&\n"
             "        hos_oot_is_player(c, c->x[19]) &&\n"
             "        c->x[13] == recomp_load64(c,c->x[19] + 7184) + 8)\n"
             "        c->x[12] = c->x[11];\n")
    body = source[start:end].replace(LIFE_CAS, guard + LIFE_CAS)
    return (source[:start] + body + source[end:]).replace(
        '#include "a64.h"\n', '#include "a64.h"\nextern int hos_oot_hearts_enabled(void);\n'
        'extern int hos_oot_is_player(Ctx *c, uint64_t actor);\n', 1)


def patch_movement(source):
    start = source.find(MOVE_HEAD)
    if start < 0:
        return None
    end = source.find("\n}", start)
    if end < 0 or source[start:end].count(MOVE_ANCHOR) != 1 or source[start:end].count(MOVE_MATRIX) != 1:
        return None
    bypass = ("    hos_oot_lift_player(c, c->x[19]);\n"
              + MOVE_ANCHOR)
    body = source[start:end].replace(MOVE_ANCHOR, bypass, 1).replace(
        MOVE_MATRIX, MOVE_MATRIX + "    hos_oot_flight_pose(c, c->x[19], c->x[25]);\n", 1)
    source = source[:start] + body + source[end:]
    return source.replace('#include "a64.h"\n', '#include "a64.h"\n'
                          'extern void hos_oot_lift_player(Ctx *c, uint64_t actor);\n'
                          'extern void hos_oot_flight_pose(Ctx *c, uint64_t actor, uint64_t matrix);\n', 1)


def patch_flight_rotation(source):
    start = source.find(WARP_HEAD)
    if start < 0:
        return None
    end = source.find("\n}", start)
    body = source[start:end]
    if end < 0 or body.count(WARP_ANCHOR) != 1:
        return None
    body = body.replace(WARP_ANCHOR, "    hos_oot_flight_rotation(c, c->x[0], c->x[1]);\n" + WARP_ANCHOR)
    return (source[:start] + body + source[end:]).replace(
        '#include "a64.h"\n', '#include "a64.h"\n'
        'extern void hos_oot_flight_rotation(Ctx *c, uint64_t controller, uint64_t matrix);\n', 1)


def patch_view(source):
    start = source.find(VIEW_HEAD)
    if start < 0:
        return None
    end = source.find("\n}", start)
    body = source[start:end]
    if end < 0:
        return None
    for anchor in VIEW_ANCHORS:
        if body.count(anchor) == 1:
            break
    else:
        return None
    body = body.replace(anchor, anchor + "    hos_oot_free_view(c, c->x[20]);\n")
    return (source[:start] + body + source[end:]).replace(
        '#include "a64.h"\n', '#include "a64.h"\nextern void hos_oot_free_view(Ctx *c, uint64_t view);\n', 1)
```

### The host runtime (`host/hos_main.c`)

```c
int hos_oot_protect_hearts;
volatile int hos_oot_live_action;   /* (test runs) the live input file's "!cheat <action>": host/nn_hid.c */

/* (hos_oot_hearts_enabled, hos_oot_lift_player, hos_oot_flight_pose, hos_oot_flight_rotation, hos_oot_free_view: called
   from nx2/oot_parked.py's patches, parked while the present lifter is retired: the switches do nothing meanwhile) */
int hos_oot_hearts_enabled(void) {
    int rx_option(int id, int otherwise);
    const int menu = rx_option(HOS_OPT_CHEAT_HEARTS, -1);
    return menu < 0 ? hos_oot_protect_hearts : menu;
}

int hos_oot_is_player(Ctx *c, uint64_t actor) {
    if (recomp_load64(c, actor) != 0x84b6c460ULL) return 0;
    // Other actors share this vtable. The actor's resource record identifies Link.
    const uint64_t resource = recomp_load64(c, actor + 16);
    return resource && recomp_load32(c, resource + 88) == 0x79616c50 &&
           recomp_load16(c, resource + 92) == 0x7265 &&
           recomp_load8(c, resource + 94) == 0;
}

#if defined(_WIN32) || defined(__linux__)   /* (the debug tools: host/posix has what they call) */
/* OneShotAddOrRemovePouchContent (f_828d4a70) for one pouch content by actor name: delta > 0 adds, < 0 removes.
   Returns the event status (top bits of the result word), or -1 when the pouch is not ready. */
static int oot_pouch_change(Ctx *c, const char *name, int delta) {
    const uint64_t pouch_singleton = recomp_load64(c, 0x84c9e0d0ULL);
    const uint64_t state_singleton = recomp_load64(c, 0x84c9e2a8ULL);
    const uint64_t actor_db = recomp_load64(c, 0x84eb6888ULL);
    const uint64_t actor_info = recomp_load64(c, 0x84e8c060ULL);
    A64Fn edit = a64_lookup(0x828d4a70ULL);
    if (!edit || !pouch_singleton || !recomp_load64(c, pouch_singleton) ||
        !state_singleton || !recomp_load64(c, state_singleton) || !actor_db || !actor_info || !delta ||
        strlen(name) >= 0x60) return -1;

    // OneShotAddOrRemovePouchContent reads the event parameters through pointers.
    const Ctx saved = *c;
    const uint64_t event = (c->x[31] - 0xe0) & ~0xfULL;
    memset(A64_PTR(event), 0, 0xe0);
    memcpy(A64_PTR(event + 0x80), name, strlen(name) + 1);
    recomp_store64_sp(c, event + 0x20, event + 0x60);
    recomp_store64_sp(c, event + 0x30, event + 0x70);
    recomp_store64_sp(c, event + 0x38, event + 0x68);
    recomp_store64_sp(c, event + 0x48, event + 0x74);
    recomp_store64_sp(c, event + 0x50, event + 0x74);
    recomp_store64_sp(c, event + 0x60, event + 0x80);
    recomp_store64_sp(c, event + 0x68, 0x83f0a6d8ULL); // Script
    recomp_store32_sp(c, event + 0x70, (uint32_t)delta);
    c->x[31] = event;
    c->x[0] = event;
    edit(c);
    const int result = (int)(recomp_load32(c, event + 0x18) >> 30);
    *c = saved;
    return result;
}

/* QueryGetPouchContentNumByActorName (f_82a3c260): f_805b63e0(pouch manager, &name, 0, 0) counts one content. */
static int oot_pouch_count(Ctx *c, const char *name) {
    const uint64_t manager = recomp_load64(c, 0x84e98e28ULL);
    A64Fn count = a64_lookup(0x805b63e0ULL);
    if (!manager || !count || strlen(name) >= 0x60) return -1;
    const Ctx saved = *c;
    const uint64_t frame = (c->x[31] - 0x80) & ~0xfULL;
    memcpy(A64_PTR(frame + 0x10), name, strlen(name) + 1);
    recomp_store64_sp(c, frame, frame + 0x10);
    c->x[31] = frame;
    c->x[0] = manager, c->x[1] = frame, c->x[2] = 0, c->x[3] = 0;
    count(c);
    const int n = (int)(int32_t)c->x[0];
    *c = saved;
    return n;
}

static void oot_edit_pouch(Ctx *c, int action) {
    static const char *const items[] = {
        "Item_DekuSeed", "Item_DekuNut", "Item_DekuStick", "Item_Bomb", "Item_Arrow_Normal"
    };
    const unsigned item = (unsigned)(action - 200) / 32;
    const int delta = (action - 200) % 32 - 10;
    if (item >= sizeof items / sizeof items[0] || !delta || delta < -10 || delta > 10) return;
    const int result = oot_pouch_change(c, items[item], delta);
    if (result >= 0) hos_log("cheats: pouch %s %+d (event status %d)", items[item], delta, result);
}

/* The Inventory page (host/oot_cheat_items.h): counts go to the menu twice a second, edits come back as actions. */
#define OOT_ITEM_NAME(label, name, kind) name,
#define OOT_ITEM_KIND(label, name, kind) kind,
#define OOT_NO_SECTION(name)
static const char *const oot_item_names[] = {OOT_CHEAT_ITEMS(OOT_NO_SECTION, OOT_ITEM_NAME)};
static const unsigned char oot_item_kinds[] = {OOT_CHEAT_ITEMS(OOT_NO_SECTION, OOT_ITEM_KIND)};
#define OOT_ITEM_COUNT (sizeof oot_item_names / sizeof oot_item_names[0])

static int oot_item_count(Ctx *c, const char *name);

static void oot_inventory_refresh(Ctx *c, int64_t now) {
    void rx_option_set(int id, int value);
    static int64_t next;
    static int probed;
    if (now < next) return;
    next = now + 500000000LL;
    for (unsigned i = 0; i < OOT_ITEM_COUNT; i++) rx_option_set(OOT_CHEAT_ITEM_BASE + (int)i, oot_item_count(c, oot_item_names[i]));
    if (!probed && oot_pouch_count(c, "Item_DekuSeed") >= 0) {
        probed = 1;   /* (one line naming every count, both spellings: which names the pouch knows) */
        static const char *const probes[] = {OOT_CHEAT_ITEM_PROBES};
        const unsigned total = (unsigned)(OOT_ITEM_COUNT + sizeof probes / sizeof probes[0]);
        char line[512];
        size_t used = 0;
        for (unsigned i = 0; i < total; i++) {   /* (several short lines: the log cuts long ones) */
            const char *name = i < OOT_ITEM_COUNT ? oot_item_names[i] : probes[i - OOT_ITEM_COUNT];
            used += (size_t)snprintf(line + used, sizeof line - used, " %s=%d", name, oot_pouch_count(c, name));
            if (used > 300 || i + 1 == total) hos_log("cheats: inventory probe:%s", line), used = 0;
        }
    }
}

/* Masks share one slot (the Happy Mask Shop lends one at a time). The game takes "Item_Mask_X" when adding one and
   may keep it as "Armor_Child_Mask_X", so both spellings count and a new mask first returns the one held. */
static int oot_mask_alias(const char *name, char *alias, size_t size) {
    if (!strncmp(name, "Armor_Child_Mask_", 17)) return snprintf(alias, size, "Item_Mask_%s", name + 17), 1;
    return 0;
}

static int oot_item_count(Ctx *c, const char *name) {
    char alias[64];
    const int n = oot_pouch_count(c, name);
    if (!oot_mask_alias(name, alias, sizeof alias)) return n;
    const int m = oot_pouch_count(c, alias);
    return n > m ? n : m;
}

static void oot_inventory_edit(Ctx *c, int action) {
    const unsigned item = (unsigned)(action - OOT_CHEAT_ITEM_ACTION) / 256, op = (unsigned)(action - OOT_CHEAT_ITEM_ACTION) % 256;
    if (item >= OOT_ITEM_COUNT) return;
    const char *name = oot_item_names[item];
    char alias[64];
    const int is_mask = oot_mask_alias(name, alias, sizeof alias);
    const int have = oot_item_count(c, name);
    int delta = 0;
    if (op == 200) delta = have > 0 ? -have : 1;   /* unlock / relock */
    else if (op < 128) delta = (int)op - 64;
    if (!delta || have < 0) return;
    if (is_mask && delta > 0) {   /* return whichever mask is in the slot first */
        for (unsigned i = 0; i < OOT_ITEM_COUNT; i++) {
            char other_alias[64];
            if (i == item || !oot_mask_alias(oot_item_names[i], other_alias, sizeof other_alias)) continue;
            const char *spellings[2] = {oot_item_names[i], other_alias};
            for (unsigned k = 0; k < 2; k++) {
                const int held = oot_pouch_count(c, spellings[k]);
                if (held > 0) hos_log("cheats: inventory returned %s (status %d)", spellings[k], oot_pouch_change(c, spellings[k], -held));
            }
        }
        delta = 1;
    }
    int result = -1;
    if (is_mask && delta < 0) {   /* remove under whichever spelling holds it */
        const char *spellings[2] = {name, alias};
        for (unsigned k = 0; k < 2; k++) {
            const int held = oot_pouch_count(c, spellings[k]);
            if (held > 0) result = oot_pouch_change(c, spellings[k], -held);
        }
    } else {
        result = oot_pouch_change(c, is_mask ? alias : name, delta);   /* (masks are added as Item_Mask_X, as the shop does) */
    }
    hos_log("cheats: inventory %s %+d (had %d, now %d, event status %d)", name, delta, have, oot_item_count(c, name), result);
}

static void oot_set_time(Ctx *c, unsigned hour) {
    // ExecuteWmSetTime passes seconds since midnight to the world manager.
    const uint64_t singleton = recomp_load64(c, 0x84c9e898ULL);
    const uint64_t world = singleton ? recomp_load64(c, singleton) : 0;
    A64Fn set_time = a64_lookup(0x834722d0ULL);
    if (!set_time || !world || !recomp_load64(c, world + 0x510)) {
        hos_log("cheats: world clock unavailable");
        return;
    }
    const Ctx saved = *c;
    c->x[0] = world;
    c->x[1] = hour * 3600;
    set_time(c);
    *c = saved;
    hos_log("cheats: requested %02u:00", hour);
}

/* Cross-map teleport (build e0f3ccfa). Map changes in this game only run through an event: an entrance actor
   requests the Dm_SY_BancChangeGo event with NextBanc / NextStartPos / BootEventName arguments, the event calls
   GameSystemActor's ReserveBancChange, and the sequence's ChangeBancOnEvent loads the map when the event ends.
   This builds the same event request (see work/oot-teleport-research.md). The menu labels in gpu/legacy/vk/vk_menu.cpp
   (teleport_names) follow this table's order. */
typedef struct {
    const char *child_banc, *child_start, *child_boot;
    const char *adult_banc, *adult_start, *adult_boot;
} OotDestination;
static const OotDestination oot_destinations[] = {   /* (generated: work/oot-teleport-tools/write_destinations.py) */
    {"KokiriForest_Child", "LostWoodBridge_FrontOfHyruleField", "Dm_SY_BancChangeArrive",
     "KokiriForest_Adult", "LostWoodBridge_FrontOfHyruleField", "Dm_SY_BancChangeArrive"},   /* Kokiri Forest */
    {"HyruleField_Child", "HyruleField_FrontOfLostWoodBridge", "Dm_SY_BancChangeArrive",
     "HyruleField_Adult", "HyruleField_FrontOfLostWoodBridge", "Dm_SY_BancChangeArrive"},   /* Hyrule Field */
    {"KakarikoVillage_Child", "KakarikoVillage_Entrance", "Dm_SY_BancChangeArrive_KakarikoVillage_01",
     "KakarikoVillage_Adult", "KakarikoVillage_Entrance", "Dm_SY_BancChangeArrive_KakarikoVillage_01"},   /* Kakariko Village */
    {"Market_Child", "Market_Entrance", "Dm_SY_BancChangeArrive_Market_02",
     "Market_Adult", "Market_Entrance", "Dm_SY_BancChangeArrive_Market_02"},   /* Market */
    {"HyruleCastle", "HyruleCastle_Entrance", "Dm_SY_BancChangeArrive_HyruleCastle_01",
     "GanonCastle", "GanonCastleEntrance", "Dm_SY_BancChangeArrive_GanonCastle_01"},   /* Hyrule Castle */
    {"LostWood_Child", "LostWood_FrontOfSacredForestMeadow", "Dm_SY_BancChangeArrive_LostWood",
     "LostWood_Adult", "LostWood_FrontOfSacredForestMeadow", "Dm_SY_BancChangeArrive_LostWood"},   /* Lost Woods */
    {"SacredForestMeadow_Child", "OutsideOfForestTemple", "Dm_SY_BancChangeArrive_SacredForestMeadow_01",
     "SacredForestMeadow_Adult", "OutsideOfForestTemple", "Dm_SY_BancChangeArrive_SacredForestMeadow_01"},   /* Sacred Forest Meadow */
    {"HyruleField_Child", "LakeHylia_FromGerudoValley", "Dm_SY_BancChangeArrive_LakeHylia_02",
     "HyruleField_Adult", "LakeHylia_FromGerudoValley", "Dm_SY_BancChangeArrive_LakeHylia_02"},   /* Lake Hylia */
    {"Graveyard_Child", "GraveYard_KakarikoSide", "Dm_SY_BancChangeArrive_Graveyard_01",
     "Graveyard_Adult", "GraveYard_KakarikoSide", "Dm_SY_BancChangeArrive_Graveyard_01"},   /* Graveyard */
    {"DeathMountainTrail_Child", "MountainTrail_KakarikoSide", "Dm_SY_BancChangeArrive_DeathMountainTrail_01",
     "DeathMountainTrail_Adult", "MountainTrail_KakarikoSide", "Dm_SY_BancChangeArrive_DeathMountainTrail_01"},   /* Death Mountain Trail */
    {"DeathMountainCrater_Child", "Crater_GoronCitySide", "DeathMountainCrater_Arrive",
     "DeathMountainCrater_Adult", "Crater_GoronCitySide", "DeathMountainCrater_Arrive"},   /* Death Mountain Crater */
    {"GoronCity_Child", "GoronCity_MountainTrailSide", "Dm_SY_BancChangeArrive_GoronCity_01",
     "GoronCity_Adult", "GoronCity_MountainTrailSide", "Dm_SY_BancChangeArrive_GoronCity_01"},   /* Goron City */
    {"ZoraRiver_Child", "ZoraRiver_Entrance", "Dm_SY_BancChangeArrive",
     "ZoraRiver_Adult", "ZoraRiver_Entrance", "Dm_SY_BancChangeArrive"},   /* Zora's River */
    {"ZoraDomain_Child", "ZoraDomain_FromZoraRiver", "Dm_SY_BancChangeArrive",
     "ZoraDomain_Adult", "ZoraDomain_FromZoraRiver", "Dm_SY_BancChangeArrive"},   /* Zora's Domain */
    {"ZoraFountain_Child", "ZoraFountain_FromZoraDomain", "Dm_SY_BancChangeArrive",
     "ZoraFountain_Adult", "ZoraFountain_FromZoraDomain", "Dm_SY_BancChangeArrive"},   /* Zora's Fountain */
    {"GerudoValley_Child", "GerudoValley_Entrance", "FromHyruleField_ArriveGerudoValley",
     "GerudoValley_Adult", "GerudoValley_Entrance", "FromHyruleField_ArriveGerudoValley"},   /* Gerudo Valley */
    {NULL, NULL, NULL,
     "GerudoFortress_Adult", "GerudoFortress_FromHountedWasteland", "Dm_SY_BancChangeArrive_GerudoFortress_02"},   /* Gerudo Fortress */
    {"HountedWasteland_Child", "HountedWasteland_FromGerudoFortress", "FromGerudoFortress_ArriveWateland",
     "HountedWasteland_Adult", "HountedWasteland_FromGerudoFortress", "FromGerudoFortress_ArriveWateland"},   /* Haunted Wasteland */
    {"DesertColossus_Child", "DesertColossus_FromHountedWasteland", "Dm_SY_BancChangeArrive_DesertColossus_01",
     "DesertColossus_Adult", "DesertColossus_FromHountedWasteland", "Dm_SY_BancChangeArrive_DesertColossus_01"},   /* Desert Colossus */
    {"InsideTheDekuTree", "Entrance", "Dm_SY_BancChangeArrive_InsideTheDekuTree_01",
     "InsideTheDekuTree", "Entrance", "Dm_SY_BancChangeArrive_InsideTheDekuTree_01"},   /* Inside the Deku Tree */
    {"DodongoCavern", "DodongoCavern_MountainTrailSide", "Dm_SY_BancChangeArrive",
     "DodongoCavern", "DodongoCavern_MountainTrailSide", "Dm_SY_BancChangeArrive"},   /* Dodongo's Cavern */
    {"InsideJabuJabuBelly", "InsideJabuJabuBelly_FromZoraFountain", "Dm_SY_BancChangeArrive",
     "InsideJabuJabuBelly", "InsideJabuJabuBelly_FromZoraFountain", "Dm_SY_BancChangeArrive"},   /* Jabu-Jabu's Belly */
    {"ForestTemple", "ForestTemple", "Dm_SY_BancChangeArrive",
     "ForestTemple", "ForestTemple", "Dm_SY_BancChangeArrive"},   /* Forest Temple */
    {"FireTemple", "FireTemple", "Dm_SY_BancChangeArrive",
     "FireTemple", "FireTemple", "Dm_SY_BancChangeArrive"},   /* Fire Temple */
    {"WaterTemple", "WaterTemple_FromLakeHylia", "SageOfWater_Arrival_WaterTemple",
     "WaterTemple", "WaterTemple_FromLakeHylia", "SageOfWater_Arrival_WaterTemple"},   /* Water Temple */
    {"ShadowTemple", "Entrance", "ShadowTemple_Arrive",
     "ShadowTemple", "Entrance", "ShadowTemple_Arrive"},   /* Shadow Temple */
    {"SpiritTemple", "SpiritTemple_FromRightHand", "Dm_SY_BancChangeArrive",
     "SpiritTemple", "SpiritTemple_FromRightHand", "Dm_SY_BancChangeArrive"},   /* Spirit Temple */
    {"BottomOfTheWell", "Well_KakarikoSide", "Dm_SY_BancChangeArrive",
     "BottomOfTheWell", "Well_KakarikoSide", "Dm_SY_BancChangeArrive"},   /* Bottom of the Well */
    {"IceCavern", "IceCavern_FromZoraFountain", "SageOfWater_Arrival_IceCavern",
     "IceCavern", "IceCavern_FromZoraFountain", "SageOfWater_Arrival_IceCavern"},   /* Ice Cavern */
    {"GerudoTrainingGround", "GerudoTrainingGround_FromGerudoFortress", "FromGerudoFortress_ArriveTrainingGround",
     "GerudoTrainingGround", "GerudoTrainingGround_FromGerudoFortress", "FromGerudoFortress_ArriveTrainingGround"},   /* Gerudo Training Ground */
    {"InsideGanonCastle", "Entrance", "Dm_SY_BancChangeArrive_InsideGanonCastle_01",
     "InsideGanonCastle", "Entrance", "Dm_SY_BancChangeArrive_InsideGanonCastle_01"},   /* Ganon's Castle */
};
#define OOT_AGE_ACTION 999   /* (the Cheats page's Age row) */
#define OOT_TELEPORT_ACTION 1000   /* (pouch edits use 200 to 348) */
#define OOT_GAMEDATA_SLOT 0x84c9e180ULL   /* GameDataMgr, as ReserveBancChange's readers load it */
#define OOT_EVENTMGR_SLOT 0x84c9e090ULL   /* EventMgr, as SeqOneShotRequestEvent loads it */
#define OOT_EVENT_ARG_SIZE 0x1b20ULL      /* EventRequestArg: SeqOneShotRequestEvent reserves this much stack */

static uint64_t oot_singleton(Ctx *c, uint64_t slot) {
    const uint64_t holder = recomp_load64(c, slot);
    return holder ? recomp_load64(c, holder) : 0;
}

/* GameDataMgr getters by MurMur3 name hash: f_8022d9c0 (bool) and f_80cd0ea0 (String64). 1 when found. */
static int oot_gamedata_bool(Ctx *c, uint32_t hash, int *value) {
    const uint64_t mgr = oot_singleton(c, OOT_GAMEDATA_SLOT);
    A64Fn get = a64_lookup(0x8022d9c0ULL);
    if (!mgr || !get) return 0;
    const Ctx saved = *c;
    const uint64_t out = (c->x[31] - 0x40) & ~0xfULL;
    recomp_store64_sp(c, out, 0);
    c->x[31] = out;
    c->x[0] = mgr, c->x[1] = out, c->x[2] = hash;
    get(c);
    const int found = (int)(c->x[0] & 1);
    *value = recomp_load8(c, out) & 1;
    *c = saved;
    return found;
}

static int oot_gamedata_string(Ctx *c, uint32_t hash, char *buf, size_t size) {
    const uint64_t mgr = oot_singleton(c, OOT_GAMEDATA_SLOT);
    A64Fn get = a64_lookup(0x80cd0ea0ULL);
    buf[0] = 0;
    if (!mgr || !get) return 0;
    const Ctx saved = *c;
    const uint64_t out = (c->x[31] - 0x40) & ~0xfULL;
    recomp_store64_sp(c, out, 0);
    c->x[31] = out;
    c->x[0] = mgr, c->x[1] = out, c->x[2] = hash;
    get(c);
    const int found = (int)(c->x[0] & 1);
    const uint64_t text = recomp_load64(c, out);
    *c = saved;
    if (!found || !text) return 0;
    for (size_t i = 0; i + 1 < size; i++) {
        buf[i] = (char)recomp_load8(c, text + i);
        buf[i + 1] = 0;
        if (!buf[i]) break;
    }
    return 1;
}

/* One argument of an EventRequestArg: the array of 200-byte entries at arg+8, capacity arg+0, count arg+16.
   Entry: +0 type (1 string pointer, 4 bool), +8 name buffer, +16 its capacity, +152 the value. As
   QueryBancChangeEventRequestArg (f_829df380) fills them. A string value is a pointer the request does not copy. */
static int oot_event_arg(Ctx *c, uint64_t arg, const char *name, uint32_t type, uint64_t value) {
    const uint32_t capacity = recomp_load32(c, arg), count = recomp_load32(c, arg + 16);
    if (count >= capacity) return 0;
    const uint64_t entry = recomp_load64(c, arg + 8) + (uint64_t)count * 200;
    const uint64_t name_buf = recomp_load64(c, entry + 8);
    const uint32_t name_cap = recomp_load32(c, entry + 16);
    const size_t length = strlen(name);
    if (!name_buf || length + 1 > name_cap) return 0;
    memcpy(A64_PTR(name_buf), name, length + 1);
    if (type == 4) recomp_store8(c, entry + 152, value & 1);
    else recomp_store64(c, entry + 152, value);
    recomp_store32(c, entry, type);
    recomp_store32(c, arg + 16, count + 1);
    return 1;
}

static struct {
    int64_t requested_at, check_at;
    unsigned destination;
} teleport;

static int oot_request_banc(Ctx *c, uint64_t actor, const char *banc, const char *start, const char *boot, int child,
                            unsigned index);

static int oot_teleport(Ctx *c, uint64_t actor, unsigned index) {
    if (index >= sizeof oot_destinations / sizeof oot_destinations[0]) return 0;
    int child = 1;
    if (!oot_gamedata_bool(c, 0xcea220d3u, &child)) hos_log("cheats: Sequence_IsChildhood not found, using child maps");
    const OotDestination *d = &oot_destinations[index];
    const char *banc = child ? d->child_banc : d->adult_banc;
    if (!banc) {
        hos_log("cheats: teleport %u has no %s map", index, child ? "child" : "adult");
        return 0;
    }
    return oot_request_banc(c, actor, banc, child ? d->child_start : d->adult_start, child ? d->child_boot : d->adult_boot,
                            child, index);
}

/* (test runs) the map change asked last, until the game reports it loaded: "scene: ready" in the log, which the scene
   cycle (work/proto/scenecycle) waits for. Set by oot_request_banc, checked on each player update. */
static struct { char banc[96]; int64_t at, seen; int armed, loaded; uint64_t actor; } scene_wait;

/* The map change itself: banc, start position (empty: the banc's default) and boot event, as the entrance actors send it. */
static int oot_request_banc(Ctx *c, uint64_t actor, const char *banc, const char *start, const char *boot, int child,
                            unsigned index) {
    const int64_t now = hos_now_ns();
    if (teleport.requested_at && now - teleport.requested_at < 8000000000LL) {
        hos_log("cheats: teleport ignored, a request is still in progress");
        return 0;
    }
    const uint64_t events = oot_singleton(c, OOT_EVENTMGR_SLOT);
    A64Fn make_arg = a64_lookup(0x80d99f10ULL), request = a64_lookup(0x80d9a820ULL);
    A64Fn set_link = a64_lookup(0x802dc440ULL), release_link = a64_lookup(0x8022b6f0ULL);
    if (!events || !make_arg || !request || !set_link || !release_link) {
        hos_log("cheats: teleport unavailable (event manager %#llx)", (unsigned long long)events);
        return 0;
    }
    if (recomp_load64(c, events + 104)) {   /* (the Main sequence's "Event" test: an event is playing) */
        hos_log("cheats: teleport waits until the current event ends");
        return 0;
    }
    int loading = 0;
    if (oot_gamedata_bool(c, 0xef2b60c2u, &loading) && loading) {   /* Sequence_IsLoadingBanc */
        hos_log("cheats: teleport ignored while a map loads");
        return 0;
    }
    if (strlen(banc) >= 128 || strlen(start) >= 128 || strlen(boot) >= 256) return 0;

    /* The event reads its string arguments after this request returns, so they live in memory that stays. */
    static uint64_t strings;
    if (!strings) strings = hos_sys_alloc(512, 16);
    if (!strings) return 0;
    memset(A64_PTR(strings), 0, 512);
    memcpy(A64_PTR(strings), banc, strlen(banc) + 1);
    memcpy(A64_PTR(strings + 128), start, strlen(start) + 1);
    memcpy(A64_PTR(strings + 256), boot, strlen(boot) + 1);

    const char *event_env = getenv("HOS_OOT_TELEPORT_EVENT");
    const char *event = event_env && *event_env ? event_env : "Dm_SY_BancChangeGo";
    const char *starter_env = getenv("HOS_OOT_TELEPORT_STARTER");
    const int with_starter = !(starter_env && !strcmp(starter_env, "0"));

    const Ctx saved = *c;
    const uint64_t arg = (c->x[31] - OOT_EVENT_ARG_SIZE - 0x100) & ~0xfULL;
    c->x[31] = arg;
    c->x[0] = arg;
    make_arg(c);
    int ok = 1;
    {   /* the event's name: arg+6800 points at its buffer, arg+6808 the capacity */
        const uint64_t buf = recomp_load64(c, arg + 6800);
        const uint32_t cap = recomp_load32(c, arg + 6808);
        if (!buf || strlen(event) + 1 > cap) ok = 0;
        else memcpy(A64_PTR(buf), event, strlen(event) + 1);
    }
    ok = ok && oot_event_arg(c, arg, "NextBanc", 1, strings);
    ok = ok && oot_event_arg(c, arg, "NextStartPos", 1, strings + 128);
    ok = ok && oot_event_arg(c, arg, "BootEventName", 1, strings + 256);
    ok = ok && oot_event_arg(c, arg, "IsCheckAir", 4, 0);
    if (ok && with_starter) {   /* the starter actor (arg+0x1950), as OneShotEventMemberRequestEventByEventName sets it */
        c->x[31] = arg;
        c->x[0] = arg + 0x1950, c->x[1] = actor, c->x[2] = 0;
        set_link(c);
    }
    uint64_t result = 0;
    if (ok) {
        c->x[31] = arg;
        c->x[0] = events, c->x[1] = arg;
        request(c);
        result = c->x[0];
    }
    /* SeqOneShotRequestEvent's cleanup of the arg's two actor links */
    for (unsigned i = 0; i < 2; i++) {
        const uint64_t link = arg + (i ? 0x1950 : 0x1968);
        recomp_store64(c, link, 0x84994b20ULL);
        c->x[31] = arg;
        c->x[0] = link;
        release_link(c);
    }
    *c = saved;
    teleport.requested_at = now;
    teleport.check_at = now + 1500000000LL;
    teleport.destination = index;
    if (ok) {
        snprintf(scene_wait.banc, sizeof scene_wait.banc, "%s", banc);
        scene_wait.at = now, scene_wait.seen = now, scene_wait.armed = 1, scene_wait.loaded = 0, scene_wait.actor = actor;
    }
    hos_log("cheats: teleport %s to %s at %s (boot %s, event %s, starter %d): request %s, result %#llx",
            child ? "child" : "adult", banc, start, boot, event, with_starter, ok ? "sent" : "not built",
            (unsigned long long)result);
    return ok;   /* (sent; the request's own result is only logged) */
}

/* Writes a GameData bool directly: GameDataMgr+32 is the (hash, index) table (+44 its size), the bools are 24-byte
   entries at GameDataMgr+24 (+16 their count) with the value at +22, as f_8022d9c0 reads them. 1 when found. */
static int oot_gamedata_set_bool(Ctx *c, uint32_t hash, int value) {
    const uint64_t mgr = oot_singleton(c, OOT_GAMEDATA_SLOT);
    if (!mgr) return 0;
    const uint64_t table = recomp_load64(c, mgr + 32);
    const uint32_t size = recomp_load32(c, mgr + 44);
    if (!table || !size) return 0;
    for (uint32_t probe = 0, slot = hash % size; probe < size; probe++, slot = (slot + 1) % size) {
        const uint32_t key = recomp_load32(c, table + slot * 8ULL);
        if (!key) return 0;
        if (key != hash) continue;
        const uint32_t index = recomp_load32(c, table + slot * 8ULL + 4), count = recomp_load32(c, mgr + 16);
        if (index >= count) return 0;
        recomp_store8(c, recomp_load64(c, mgr + 24) + index * 24ULL + 22, value ? 1 : 0);
        return 1;
    }
    return 0;
}

/* Age toggle: flips Sequence_IsChildhood (what the Temple of Time event sets) and reloads the current map, or the
   Market when this map is not in oot_destinations, through the teleport so the other age's map and Link load. */
static void oot_toggle_age(Ctx *c, uint64_t actor) {
    int child = 1;
    if (!oot_gamedata_bool(c, 0xcea220d3u, &child)) {
        hos_log("cheats: age toggle: Sequence_IsChildhood not found");
        return;
    }
    char current[80];
    oot_gamedata_string(c, 0x1d6189dau, current, sizeof current);   /* Sequence_CurrentBanc */
    unsigned target = 3;   /* Market (the Temple of Time is in it) */
    for (unsigned i = 0; i < sizeof oot_destinations / sizeof oot_destinations[0]; i++) {
        const OotDestination *d = &oot_destinations[i];
        const char *other = child ? d->adult_banc : d->child_banc;
        if (other && ((d->child_banc && !strcmp(d->child_banc, current)) || (d->adult_banc && !strcmp(d->adult_banc, current)))) {
            target = i;
            break;
        }
    }
    if (!oot_gamedata_set_bool(c, 0xcea220d3u, !child)) {
        hos_log("cheats: age toggle: could not write Sequence_IsChildhood");
        return;
    }
    hos_log("cheats: age toggle: %s -> %s, reloading via destination %u (current map \"%s\")",
            child ? "child" : "adult", child ? "adult" : "child", target, current);
    if (!oot_teleport(c, actor, target)) {
        oot_gamedata_set_bool(c, 0xcea220d3u, child);
        hos_log("cheats: age toggle undone, the map change was not requested");
    }
}

static void oot_scene_ready_check(Ctx *c, uint64_t actor) {
    if (!scene_wait.armed) return;
    const int64_t now = hos_now_ns();
    if (now - scene_wait.at > 120000000000LL) {
        hos_log("scene: not ready after 120 s: %s", scene_wait.banc);
        scene_wait.armed = 0;
        return;
    }
    int loading = 0;
    if (oot_gamedata_bool(c, 0xef2b60c2u, &loading) && loading) {   /* Sequence_IsLoadingBanc */
        scene_wait.loaded = 1;
        return;
    }
    if (actor != scene_wait.actor) scene_wait.loaded = 1;   /* (a new Link: the map was made again) */
    if (now - scene_wait.seen > 300000000LL) scene_wait.loaded = 1;   /* (no player update for a while: a load ran) */
    scene_wait.seen = now;
    if (!scene_wait.loaded) return;   /* (the same banc again: its name matches before the load) */
    const uint64_t events = oot_singleton(c, OOT_EVENTMGR_SLOT);
    if (events && recomp_load64(c, events + 104)) return;   /* the arrival event still plays */
    char current[96];
    oot_gamedata_string(c, 0x1d6189dau, current, sizeof current);   /* Sequence_CurrentBanc */
    if (strcmp(current, scene_wait.banc)) return;
    scene_wait.armed = 0;
    teleport.requested_at = 0;   /* (the next map change may go at once: this one is done) */
    hos_log("scene: ready %s after %.2f s", current, (now - scene_wait.at) / 1e9);
}

/* (test runs) "!warp <banc> [startpos] [boot event]" from the live input file (host/nn_hid.c): any map, not only the
   teleport table's. Written by the input thread, taken by the player update. */
static char warp_line[384];
static volatile int warp_pending;
void hos_oot_warp_request(const char *args) {
    if (__atomic_load_n(&warp_pending, __ATOMIC_ACQUIRE)) return;
    snprintf(warp_line, sizeof warp_line, "%s", args);
    __atomic_store_n(&warp_pending, 1, __ATOMIC_RELEASE);
}
static void oot_warp_take(Ctx *c, uint64_t actor) {
    if (!__atomic_load_n(&warp_pending, __ATOMIC_ACQUIRE)) return;
    {   /* (quietly wait for the last change, a load or an event to finish: oot_request_banc would log each try) */
        int loading = 0;
        const uint64_t events = oot_singleton(c, OOT_EVENTMGR_SLOT);
        if ((teleport.requested_at && hos_now_ns() - teleport.requested_at < 8000000000LL) ||
            (oot_gamedata_bool(c, 0xef2b60c2u, &loading) && loading) || (events && recomp_load64(c, events + 104)))
            return;
    }
    char banc[128] = "", start[128] = "", boot[128] = "";
    sscanf(warp_line, "%127s %127s %127s", banc, start, boot);
    if (!strcmp(start, "-")) start[0] = 0;
    if (!boot[0]) snprintf(boot, sizeof boot, "Dm_SY_BancChangeArrive");
    else if (!strcmp(boot, "-")) boot[0] = 0;   /* (none: AfterChangeBanc plays the banc's own BootEvent) */
    int child = 1;
    oot_gamedata_bool(c, 0xcea220d3u, &child);
    /* (sent once: the request's result is 0 also when the event is taken; the scene cycle sends it again if no
       "scene: ready" follows) */
    if (banc[0]) oot_request_banc(c, actor, banc, start, boot, child, ~0u);
    __atomic_store_n(&warp_pending, 0, __ATOMIC_RELEASE);
}

static void oot_teleport_followup(Ctx *c) {
    if (!teleport.check_at || hos_now_ns() < teleport.check_at) return;
    teleport.check_at = 0;
    char next[80], current[80];
    oot_gamedata_string(c, 0xee27bf71u, next, sizeof next);     /* Sequence_ReservedNextBanc */
    oot_gamedata_string(c, 0x1d6189dau, current, sizeof current);   /* Sequence_CurrentBanc */
    const uint64_t events = oot_singleton(c, OOT_EVENTMGR_SLOT);
    hos_log("cheats: teleport check: reserved \"%s\", current \"%s\", event playing %d", next, current,
            events && recomp_load64(c, events + 104) ? 1 : 0);
}

static uint64_t oot_gameplay_view(Ctx *c) {
    const uint64_t singleton = recomp_load64(c, 0x84ca6bc0ULL);
    const uint64_t manager = singleton ? recomp_load64(c, singleton) : 0;
    const uint64_t cameras = manager && recomp_load32(c, manager + 32) ? recomp_load64(c, manager + 40) : 0;
    const uint64_t camera = cameras ? recomp_load64(c, cameras) : 0;
    return camera ? recomp_load64(c, camera + 120) : 0;
}

static float oot_load_float(Ctx *c, uint64_t addr) {
    const uint32_t bits = recomp_load32(c, addr);
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static _Thread_local uint64_t flight_transform_controller;
static _Thread_local uint64_t flight_pose_actor;
static _Thread_local float flight_transform_yaw;
static int64_t flight_player_seen;

static void oot_update_player_cheats(Ctx *c, uint64_t actor, uint64_t components) {
    int rx_option(int id, int otherwise);
    static uint64_t flying_actor, marked_actor, marked_controller;
    static int64_t last_tick, last_player_tick;
    static float target[3], velocity[3], mark[3], yaw;
    const int flying = rx_option(HOS_OPT_CHEAT_FLIGHT, 0) > 0;
    if (!flying) flying_actor = 0;
    const int64_t now = hos_now_ns();
    __atomic_store_n(&flight_player_seen, now, __ATOMIC_RELAXED);
    if (marked_actor != actor || now - last_player_tick > 2000000000LL) marked_controller = 0;
    last_player_tick = now;
    int action = rx_option(HOS_OPT_CHEAT_ACTION, 0);
    if (!action) action = __atomic_exchange_n(&hos_oot_live_action, 0, __ATOMIC_RELAXED);
    oot_teleport_followup(c);
    if (hos_bench_on()) {   /* (HOS_BENCH's "in play": no map load, no event playing) */
        int loading = 0;
        oot_gamedata_bool(c, 0xef2b60c2u, &loading);   /* Sequence_IsLoadingBanc */
        const uint64_t events = oot_singleton(c, OOT_EVENTMGR_SLOT);
        hos_bench_player(loading, events && recomp_load64(c, events + 104));
    }
    oot_scene_ready_check(c, actor);
    oot_warp_take(c, actor);
    oot_inventory_refresh(c, now);
    if (action >= OOT_CHEAT_ITEM_ACTION && action < OOT_CHEAT_ITEM_ACTION + 256 * (int)OOT_ITEM_COUNT) {
        oot_inventory_edit(c, action);
    } else if (action == OOT_AGE_ACTION) {
        oot_toggle_age(c, actor);
    } else if (action >= OOT_TELEPORT_ACTION) {
        oot_teleport(c, actor, (unsigned)(action - OOT_TELEPORT_ACTION));
    } else if (action >= 200) {
        oot_edit_pouch(c, action);
    } else if (action >= 100 && action < 124) {
        oot_set_time(c, action - 100);
    }
    if (!flying && action != 2 && action != 3) return;

    // Follow OneShotPhysicsCctChangePosition to Link's controller.
    const unsigned offsets[] = {80, 32, 16, 8};
    uint64_t controller = components;
    for (unsigned i = 0; i < sizeof offsets / sizeof offsets[0]; i++) {
        controller = recomp_load64(c, controller + offsets[i]);
        if (!controller) return;
    }
    const uint64_t vtable = recomp_load64(c, controller);
    if (vtable != 0x84c54f10ULL || recomp_load64(c, vtable + 0xe8) != 0x806d2530ULL) return;
    A64Fn change_position = a64_lookup(0x806d2530ULL);
    if (!change_position) return;

    if (action == 2) {
        for (unsigned i = 0; i < 3; i++) {
            const uint32_t bits = recomp_load32(c, actor + 796 + i * 4);
            memcpy(&mark[i], &bits, sizeof bits);
        }
        marked_actor = actor;
        marked_controller = controller;
        hos_log("cheats: marked position %.2f %.2f %.2f", mark[0], mark[1], mark[2]);
    }
    const int returning = action == 3 && marked_controller == controller;
    if (action == 3 && !returning) hos_log("cheats: no mark for this player controller");
    if (!flying && !returning) return;
    const int64_t elapsed = now - last_tick;
    if (flying_actor != actor)
        yaw = atan2f(oot_load_float(c, actor + 816), oot_load_float(c, actor + 840));
    if (returning) {
        memcpy(target, mark, sizeof target);
        memset(velocity, 0, sizeof velocity);
        flying_actor = flying ? actor : 0;
    } else if (flying_actor != actor || elapsed < 0 || elapsed > 250000000) {
        for (unsigned i = 0; i < 3; i++) target[i] = oot_load_float(c, actor + 796 + i * 4);
        memset(velocity, 0, sizeof velocity);
        flying_actor = actor;
        last_tick = now;
        return;
    } else if (elapsed == 0) {
        return;
    }
    last_tick = now;

    DWORD foreground = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
    if (!returning) {
        const float dt = fminf((float)elapsed * 1e-9f, 0.05f);
        float desired[3] = {0};
        if (foreground == GetCurrentProcessId() && !rx_option(1, 0) && !rx_option(HOS_OPT_CHEAT_CAMERA, 0)) {
            uint64_t hos_flight_input(void);
            const uint64_t axes = hos_flight_input();
            const float right = (int16_t)axes / 32767.0f, forward = (int16_t)(axes >> 16) / 32767.0f;
            const int vertical = (GetAsyncKeyState(VK_PRIOR) < 0) - (GetAsyncKeyState(VK_NEXT) < 0);
            const uint64_t view = oot_gameplay_view(c);
            float rx = 1, rz = 0;
            if (view && recomp_load64(c, view) == 0x84c18a88ULL) {
                const float x = oot_load_float(c, view + 8), z = oot_load_float(c, view + 16);
                const float length = hypotf(x, z);
                if (length > 0.01f) rx = x / length, rz = z / length;
            }
            desired[0] = rx * right + rz * forward;
            desired[1] = vertical ? vertical : (int16_t)(axes >> 32);
            desired[2] = rz * right - rx * forward;
        }
        const float length = sqrtf(desired[0] * desired[0] + desired[1] * desired[1] + desired[2] * desired[2]);
        const float speed = 4.0f / fmaxf(1.0f, length);
        const float blend = 1.0f - expf(-12.0f * dt);
        for (unsigned i = 0; i < 3; i++) {
            velocity[i] += (desired[i] * speed - velocity[i]) * blend;
            target[i] += velocity[i] * dt;
        }
        if (hypotf(desired[0], desired[2]) > 0.08f) {
            const float delta = remainderf(atan2f(desired[0], desired[2]) - yaw, 6.28318531f);
            yaw += fmaxf(-8.0f * dt, fminf(delta, 8.0f * dt));
        }
    }

    const Ctx saved = *c;
    const uint64_t arg = c->x[31] - 16;
    for (unsigned i = 0; i < 3; i++) {
        uint32_t bits;
        memcpy(&bits, &target[i], sizeof bits);
        recomp_store32_sp(c, arg + i * 4, bits);
    }
    recomp_store32_sp(c, arg + 12, 0);
    c->x[31] = arg;
    c->x[0] = controller;
    c->x[1] = arg;
    flight_transform_yaw = yaw;
    flight_pose_actor = flying && !returning ? actor : 0;
    flight_transform_controller = flying && !returning ? controller : 0;
    change_position(c);
    flight_transform_controller = 0;
    *c = saved;
    if (returning) hos_log("cheats: returned to %.2f %.2f %.2f", mark[0], mark[1], mark[2]);
}
#endif

int hos_oot_flight_shortcut_ready(void) {
#if defined(_WIN32) || defined(__linux__)   /* (the debug tools: host/posix has what they call) */
    const int64_t seen = __atomic_load_n(&flight_player_seen, __ATOMIC_RELAXED);
    return seen && hos_now_ns() - seen < 250000000;
#else
    return 0;
#endif
}

#if defined(_WIN32) || defined(__linux__)   /* (the debug tools: host/posix has what they call) */
static void oot_write_flight_rotation(Ctx *c, uint64_t matrix) {
    const float s = sinf(flight_transform_yaw), co = cosf(flight_transform_yaw);
    const float rotation[3][3] = {{co, 0, s}, {0, 1, 0}, {-s, 0, co}};
    for (unsigned row = 0; row < 3; row++) {
        for (unsigned col = 0; col < 3; col++) {
            uint32_t bits;
            memcpy(&bits, &rotation[row][col], sizeof bits);
            recomp_store32(c, matrix + row * 16 + col * 4, bits);
        }
    }
}
#endif

void hos_oot_flight_rotation(Ctx *c, uint64_t controller, uint64_t matrix) {
#if defined(_WIN32) || defined(__linux__)   /* (the debug tools: host/posix has what they call) */
    if (flight_transform_controller == controller) oot_write_flight_rotation(c, matrix);
#else
    (void)c;
    (void)controller;
    (void)matrix;
#endif
}

void hos_oot_flight_pose(Ctx *c, uint64_t actor, uint64_t matrix) {
#if defined(_WIN32) || defined(__linux__)   /* (the debug tools: host/posix has what they call) */
    if (flight_pose_actor == actor) {
        flight_pose_actor = 0;
        oot_write_flight_rotation(c, matrix);
    }
#else
    (void)c;
    (void)actor;
    (void)matrix;
#endif
}

void hos_oot_lift_player(Ctx *c, uint64_t actor) {
#if defined(_WIN32) || defined(__linux__)   /* (the debug tools: host/posix has what they call) */
    flight_pose_actor = 0;
    if (recomp_load64(c, actor) != 0x84b45e18ULL) return;
    const uint64_t components = recomp_load64(c, actor + 624);
    const uint64_t movement = components ? recomp_load64(c, components + 800) : 0;
    if (movement < HOS_HEAP_BASE + 0x1d50 || recomp_load64(c, movement + 24) != actor ||
        !hos_oot_is_player(c, movement - 0x1d50)) return;

    // Link can be updated by different guest workers on successive frames.
    static SRWLOCK cheats_lock = SRWLOCK_INIT;
    if (!TryAcquireSRWLockExclusive(&cheats_lock)) return;
    oot_update_player_cheats(c, actor, components);
    ReleaseSRWLockExclusive(&cheats_lock);
#else
    (void)c;
    (void)actor;
#endif
}

#if defined(_WIN32) || defined(__linux__)   /* (the debug tools: host/posix has what they call) */
static void oot_build_free_view(Ctx *c, uint64_t view, uint64_t out) {
    int rx_option(int id, int otherwise);
    static uint64_t active_view;
    static int64_t last_tick;
    static double eye[3], yaw, pitch;
    const int64_t now = hos_now_ns();
    const int64_t elapsed = now - last_tick;

    if (!view) {
        active_view = 0;
        return;
    }
    if (active_view != view || elapsed > 500000000) {
        double target[3];
        memcpy(eye, A64_PTR(view + 96), sizeof eye);
        memcpy(target, A64_PTR(view + 120), sizeof target);
        const double x = eye[0] - target[0], y = eye[1] - target[1], z = eye[2] - target[2];
        yaw = atan2(x, z);
        pitch = atan2(y, hypot(x, z));
        active_view = view;
        hos_log("cheats: free camera at %.2f %.2f %.2f", eye[0], eye[1], eye[2]);
    }
    last_tick = now;
    DWORD foreground = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
    const double dt = elapsed > 0 && elapsed < 100000000 ? (double)elapsed * 1e-9 : 0;
    const int control = foreground == GetCurrentProcessId() && !rx_option(1, 0);
    int height = 0;
    uint64_t hos_free_camera_input(int *height);
    const uint64_t input = control ? hos_free_camera_input(&height) : 0;
    if (control) {
        yaw -= 1.6 * dt * (int16_t)(input >> 32) / 32767.0;
        pitch -= 1.2 * dt * (int16_t)(input >> 48) / 32767.0;
        pitch = fmax(-1.55, fmin(1.55, pitch));
    }
    const double sy = sin(yaw), cy = cos(yaw), sp = sin(pitch), cp = cos(pitch);
    const double axes[3][3] = {{cy, 0, -sy}, {-sp * sy, cp, -sp * cy}, {cp * sy, sp, cp * cy}};
    if (control) {
        const double step = dt * (GetAsyncKeyState(VK_CONTROL) < 0 ? 30.0 : 8.0);
        const double right = (int16_t)input / 32767.0;
        const double forward = (int16_t)(input >> 16) / 32767.0;
        const int key_height = (GetAsyncKeyState(VK_PRIOR) < 0) - (GetAsyncKeyState(VK_NEXT) < 0);
        eye[1] += step * (key_height ? key_height : height);
        for (int i = 0; i < 3; i++)
            eye[i] += step * (right * axes[0][i] - forward * axes[2][i]);
    }
    for (int row = 0; row < 3; row++) {
        const double *axis = axes[row];
        const float values[4] = {(float)axis[0], (float)axis[1], (float)axis[2],
            (float)-(axis[0] * eye[0] + axis[1] * eye[1] + axis[2] * eye[2])};
        for (int col = 0; col < 4; col++) {
            uint32_t bits;
            memcpy(&bits, &values[col], sizeof bits);
            recomp_store32(c, out + (row * 4 + col) * 4, bits);
        }
    }
}
#endif

void hos_oot_free_view(Ctx *c, uint64_t view) {
#if defined(_WIN32) || defined(__linux__)   /* (the debug tools: host/posix has what they call) */
    int rx_option(int id, int otherwise);
    if (!view || recomp_load64(c, view) != 0x84c18a88ULL) return;

    // Camera zero supplies the gameplay view. The same matrix routine also serves other cameras.
    if (oot_gameplay_view(c) != view) return;

    static SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lock);
    const int enabled = rx_option(HOS_OPT_CHEAT_CAMERA, 0);
    oot_build_free_view(c, enabled ? view : 0, view + 8);
    ReleaseSRWLockExclusive(&lock);
#else
    (void)c;
    (void)view;
#endif
}

/* (in main) */
    {
        const char *protect_hearts = getenv("HOS_OOT_PROTECT_HEARTS");
        hos_oot_protect_hearts = protect_hearts && !strcmp(protect_hearts, "1");
    }
```

### Input (`host/nn_hid.c`)

The live commands of `HOS_INPUT_LIVE`:

```c
            if (line[0] == '!') {   /* commands: !synclog, !cheat, !warp */
                extern volatile int hos_nvn_sync_log_on;   /* nvn_native.c: "!synclog" */
                if (!strncmp(line, "!synclog", 8)) hos_nvn_sync_log_on = 1;
                extern volatile int hos_oot_live_action;   /* host/hos_main.c: "!cheat <action>", as the Cheats page sends */
                if (!strncmp(line, "!cheat ", 7)) __atomic_store_n(&hos_oot_live_action, atoi(line + 7), __ATOMIC_RELAXED);
                void hos_oot_warp_request(const char *args);   /* host/hos_main.c: "!warp <banc> [startpos|-] [boot event]" */
                if (!strncmp(line, "!warp ", 6)) {
                    char args[384];
                    snprintf(args, sizeof args, "%.*s", (int)(n > 7 ? n - 7 : 0), line + 6);
                    hos_oot_warp_request(args);
                }
                hos_log("hid: live command at %.1f s: %.*s", at / 1000.0, (int)(n > 1 ? n - 1 : 0), line);
                continue;
            }
```

Flight and free-camera input, and the double-B shortcut:

```c
static uint64_t flight_input;
static uint64_t free_camera_axes;
static int free_camera_height;

uint64_t hos_flight_input(void) {
    return __atomic_load_n(&flight_input, __ATOMIC_RELAXED);
}

uint64_t hos_free_camera_input(int *height) {
    *height = __atomic_load_n(&free_camera_height, __ATOMIC_RELAXED);
    return __atomic_load_n(&free_camera_axes, __ATOMIC_RELAXED);
}

static int flight_jump_shortcut(uint64_t *buttons, int mode, int menu_open, int free_camera) {
    int hos_oot_flight_shortcut_ready(void);
    static int64_t first_press;
    static int held, consume_jump;
    const uint64_t jump = 1ULL << B_B;
    const int down = !!(*buttons & jump);
    if (mode < 0 || menu_open || free_camera || !hos_oot_flight_shortcut_ready() || !game_in_front()) {
        first_press = 0;
        held = 1;
        consume_jump = 0;
        return mode > 0;
    }
    if (down && !held) {
        const int64_t now = hos_now_ns();
        if (first_press && now - first_press <= 300000000) {
            mode = !mode;
            rx_option_set(HOS_OPT_CHEAT_FLIGHT, mode);
            mode = rx_option(HOS_OPT_CHEAT_FLIGHT, -1);
            first_press = 0;
            consume_jump = mode == 0;
        } else {
            first_press = now;
        }
    } else if (!down) {
        consume_jump = 0;
    }
    held = down;
    if (consume_jump) *buttons &= ~jump;
    return mode > 0;
}

/* (in the npad read) */
        uint64_t axes = 0;
        uint64_t flight = 0;
        int height = 0;
        const int menu_open = rx_option(1, 0);
        const int free_camera = rx_option(HOS_OPT_CHEAT_CAMERA, 0);
        const int flying = flight_jump_shortcut(&buttons, rx_option(HOS_OPT_CHEAT_FLIGHT, -1), menu_open, free_camera);
        if (free_camera && !menu_open) {
            for (int i = 0; i < 4; i++) axes |= (uint64_t)(uint16_t)stick[i] << (16 * i);
            height = (int)(buttons >> B_R & 1) - (int)(buttons >> B_L & 1);
            memset(stick, 0, sizeof stick);
            buttons &= ~((1ULL << B_L) | (1ULL << B_R));
        } else if (flying && !menu_open) {
            const uint64_t climb = (1ULL << B_B) | (1ULL << B_R);
            const int vertical = !!(buttons & climb) - (int)(buttons >> B_L & 1);
            flight = (uint16_t)stick[0] | (uint64_t)(uint16_t)stick[1] << 16 |
                     (uint64_t)(uint16_t)vertical << 32;
            stick[0] = stick[1] = 0;
            buttons &= ~(climb | (1ULL << B_L));
        }
        __atomic_store_n(&flight_input, flight, __ATOMIC_RELAXED);
        __atomic_store_n(&free_camera_axes, axes, __ATOMIC_RELAXED);
        __atomic_store_n(&free_camera_height, height, __ATOMIC_RELAXED);
```

### The Cheats page's item list (`host/oot_cheat_items.h`) and option ids (`gpu/api/vk_api.h`)

```c
enum { HOS_OPT_CHEAT_HEARTS = 32, HOS_OPT_CHEAT_FLIGHT, HOS_OPT_CHEAT_ACTION, HOS_OPT_CHEAT_CAMERA };
/* (OoT debug cheats) the inventory editor's items, shared by the menu (gpu/legacy/vk/vk_menu.cpp) and the game side
   (host/hos_main.c). OOT_SECTION starts a heading in the menu list; OOT_ITEM(label, pouch content name, kind):
   kind 0 an item Link has or not (Enter unlocks / relocks), 1 a quantity (Left / Right change it).
   Pouch names come from the game's own event flows and actor packs (work/oot-teleport-research.md, "Inventory").
   The names marked (guess) are the likeliest of two spellings; the log's "inventory probe" line shows both. */
#ifndef OOT_CHEAT_ITEMS
#define OOT_CHEAT_ITEMS(OOT_SECTION, OOT_ITEM) \
    OOT_SECTION("Equipment") \
    OOT_ITEM("Kokiri Sword", "Weapon_Sword_Kokiri", 0) \
    OOT_ITEM("Master Sword", "Weapon_Sword_Master", 0) \
    OOT_ITEM("Giant's Knife", "Weapon_Sword_Giant", 0) \
    OOT_ITEM("Biggoron's Sword", "Weapon_Sword_Biggoron", 0) \
    OOT_ITEM("Deku Shield", "Weapon_Shield_Deku", 0) \
    OOT_ITEM("Hylian Shield", "Weapon_Shield_Hylian", 0) \
    OOT_ITEM("Mirror Shield", "Weapon_Shield_Mirror", 0) \
    OOT_ITEM("Goron Tunic", "Armor_Adult_Goron", 0) \
    OOT_ITEM("Zora Tunic", "Armor_Adult_Zora", 0) \
    OOT_ITEM("Iron Boots", "Armor_Adult_HeavyBoot", 0) \
    OOT_ITEM("Hover Boots", "Armor_Adult_HoverBoot", 0) \
    OOT_SECTION("Items") \
    OOT_ITEM("Deku Sticks", "Item_DekuStick", 1) \
    OOT_ITEM("Deku Nuts", "Item_DekuNut", 1) \
    OOT_ITEM("Bombs", "Item_Bomb", 1) \
    OOT_ITEM("Bombchus", "Item_Bombchu", 1) \
    OOT_ITEM("Fairy Slingshot", "Weapon_Range_SlingShot", 0) \
    OOT_ITEM("Deku Seeds", "Item_DekuSeed", 1) \
    OOT_ITEM("Fairy Bow", "Weapon_Range_Bow", 0) \
    OOT_ITEM("Arrows", "Item_Arrow_Normal", 1) \
    OOT_ITEM("Fire Arrow", "Item_Arrow_Fire", 0) \
    OOT_ITEM("Ice Arrow", "Item_Arrow_Ice", 0) \
    OOT_ITEM("Light Arrow", "Item_Arrow_Light", 0) \
    OOT_ITEM("Din's Fire", "Item_DinFire", 0) \
    OOT_ITEM("Farore's Wind", "Item_FaroreWind", 0) \
    OOT_ITEM("Nayru's Love", "Item_NayruLove", 0) \
    OOT_ITEM("Fairy Ocarina", "Item_Ocarina_Fairy", 0) \
    OOT_ITEM("Ocarina of Time", "Item_Ocarina_Time", 0) \
    OOT_ITEM("Hookshot", "Item_Hookshot", 0) \
    OOT_ITEM("Longshot", "Item_Longshot", 0) \
    OOT_ITEM("Boomerang", "Item_Boomerang", 0) \
    OOT_ITEM("Lens of Truth", "Item_GlassTruth", 0) \
    OOT_ITEM("Magic Beans", "Item_MagicBean", 1) \
    OOT_ITEM("Megaton Hammer", "Item_Hammer", 0) \
    OOT_ITEM("Empty Bottle", "Item_Bottle", 1) \
    OOT_SECTION("Masks") \
    OOT_ITEM("Keaton Mask", "Armor_Child_Mask_Keaton", 0) \
    OOT_ITEM("Skull Mask", "Armor_Child_Mask_Skull", 0) \
    OOT_ITEM("Spooky Mask", "Armor_Child_Mask_Spooky", 0) \
    OOT_ITEM("Bunny Hood", "Armor_Child_Mask_Bunny", 0) \
    OOT_ITEM("Goron Mask", "Armor_Child_Mask_Goron", 0) \
    OOT_ITEM("Zora Mask", "Armor_Child_Mask_Zora", 0) \
    OOT_ITEM("Gerudo Mask", "Armor_Child_Mask_Gerudo", 0) \
    OOT_ITEM("Mask of Truth", "Armor_Child_Mask_Truth", 0) \
    OOT_SECTION("Upgrades") \
    OOT_ITEM("Goron's Bracelet", "Item_Gauntlet_Goron", 0) \
    OOT_ITEM("Silver Gauntlets", "Armor_Adult_Gauntlet_Silver", 0) \
    OOT_ITEM("Golden Gauntlets", "Armor_Adult_Gauntlet_Gold", 0) \
    OOT_ITEM("Silver Scale", "Item_Scale_Silver", 0) \
    OOT_ITEM("Golden Scale", "Item_Scale_Gold", 0) \
    OOT_ITEM("Bomb Bag", "Item_BombBag_Small", 0) \
    OOT_ITEM("Big Bomb Bag", "Item_BombBag_Medium", 0) \
    OOT_ITEM("Biggest Bomb Bag", "Item_BombBag_Large", 0) \
    OOT_ITEM("Quiver", "Item_Quiver_Small", 0) \
    OOT_ITEM("Big Quiver", "Item_Quiver_Medium", 0) \
    OOT_ITEM("Biggest Quiver", "Item_Quiver_Large", 0) \
    OOT_ITEM("Bullet Bag", "Item_BulletBag_Small", 0) \
    OOT_ITEM("Big Bullet Bag", "Item_BulletBag_Medium", 0) \
    OOT_ITEM("Biggest Bullet Bag", "Item_BulletBag_Large", 0) \
    OOT_ITEM("Adult's Wallet", "Item_Wallet_Medium", 0) \
    OOT_ITEM("Giant's Wallet", "Item_Wallet_Large", 0) \
    OOT_ITEM("Magic Meter", "Item_FairyBless_MP", 0) \
    OOT_ITEM("Double Magic", "Item_FairyBless_MPDouble", 0) \
    OOT_ITEM("Double Defense", "Item_FairyBless_DefenseDouble", 0) \
    OOT_SECTION("Quest") \
    OOT_ITEM("Kokiri's Emerald", "Item_Stone_Forest", 0) \
    OOT_ITEM("Goron's Ruby", "Item_Stone_Fire", 0) \
    OOT_ITEM("Zora's Sapphire", "Item_Stone_Water", 0) \
    OOT_ITEM("Forest Medallion", "Item_Medal_Forest", 0) \
    OOT_ITEM("Fire Medallion", "Item_Medal_Fire", 0) \
    OOT_ITEM("Water Medallion", "Item_Medal_Water", 0) \
    OOT_ITEM("Shadow Medallion", "Item_Medal_Shadow", 0) \
    OOT_ITEM("Spirit Medallion", "Item_Medal_Spirit", 0) \
    OOT_ITEM("Light Medallion", "Item_Medal_Light", 0) \
    OOT_ITEM("Zelda's Lullaby", "Item_Melody_ZeldaLullaby", 0) \
    OOT_ITEM("Epona's Song", "Item_Melody_EponaSong", 0) \
    OOT_ITEM("Saria's Song", "Item_Melody_SariaSong", 0) \
    OOT_ITEM("Sun's Song", "Item_Melody_SunSong", 0) \
    OOT_ITEM("Song of Time", "Item_Melody_TimeSong", 0) \
    OOT_ITEM("Song of Storms", "Item_Melody_StormSong", 0) \
    OOT_ITEM("Minuet of Forest", "Item_Melody_ForestMinuet", 0) \
    OOT_ITEM("Bolero of Fire", "Item_Melody_FireBolero", 0) \
    OOT_ITEM("Serenade of Water", "Item_Melody_WaterSerenade", 0) \
    OOT_ITEM("Nocturne of Shadow", "Item_Melody_ShadowNocturne", 0) \
    OOT_ITEM("Requiem of Spirit", "Item_Melody_SpiritRequiem", 0) \
    OOT_ITEM("Prelude of Light", "Item_Melody_LightPrelude", 0) \
    OOT_ITEM("Stone of Agony", "Item_AgonyStone", 0) \
    OOT_ITEM("Gerudo's Card", "Item_Token_Gerudo", 0) \
    OOT_ITEM("Gold Skulltula Tokens", "Item_Token_Gold", 1)
#endif

/* (other spellings the game might use for the same item: logged once by the game side for checking) */
#define OOT_CHEAT_ITEM_PROBES \
    "Item_Sword_Kokiri", "Item_Sword_Giant", "Item_Sword_Biggoron", "Item_Shield_Mirror", "Item_Range_Bow", \
    "Item_Range_SlingShot", "Weapon_Range_Hookshot", "Weapon_Range_Longshot", "Weapon_Boomerang", "Weapon_Hammer", \
    "Item_HeavyBoot", "Item_HoverBoot", "Item_Mask_Keaton", "Item_Gauntlet_Silver", "Armor_GlassTruth", "Armor_Child_GlassTruth", "Armor_Adult_GlassTruth", \
    "Armor_Child_Gauntlet_Goron", "Item_Wallet_Small"

#define OOT_CHEAT_ITEM_BASE 4096   /* rx_option_set(OOT_CHEAT_ITEM_BASE + i, count) from the game side; -1 unknown */
#define OOT_CHEAT_ITEM_ACTION 3000 /* + item * 256 + op: op 0..127 adds op - 64; op 200 unlocks (1) or relocks (all) */
```
