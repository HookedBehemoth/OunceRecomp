"""Parked patches of the lifted C for Ocarina of Time, not applied.

Each was a text edit of a64-kit's lifted C (nx2/oot_patches.py applied them; both are deleted). The lifter writes no C,
so they wait here until each is ported to a guest-address probe (nx2/lift2/probes.py) or dropped. (Ported: the native
mouse camera and text speed, At probes.) Nothing calls this module: the lift and the build leave these functions to the
lifter unchanged, and the host's switches for them do nothing meanwhile. The table of what each did, its switch and
what is lost while parked: docs/lifter.md, "Parked patches". The text is kept as it was, as the reference for the
port.

- Player heart protection (patch_life, f_804bd8c0), player flight (patch_movement, f_82c415e0) and its rotation
  (patch_flight_rotation, f_806d2530): host/oot_cheats.c's switches.
- Free camera (patch_view, f_804e48c0's view loop): hos_oot_free_view.
- The menus' mouse (patch_menu_focus, f_802ed660; patch_menu_tick, f_802f1380): host/oot_menu.c. It was always on:
  while parked the game's menus do not follow the mouse.
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

MENU_FOCUS_HEAD = "void f_802ed660(Ctx *__restrict A64_NOESC c) {\n"


def patch_menu_focus(source):
    """f_802ed660, a menu screen's handler of a pane getting the focus (host/oot_menu.c), calls
    hos_oot_menu_focus first, with the screen, the pane and its caller's return address"""
    start = source.find(MENU_FOCUS_HEAD)
    if start < 0:
        return None
    if "hos_oot_menu_focus(c," in source:   # (made before: a lift patched again)
        return source
    at = start + len(MENU_FOCUS_HEAD)
    hook = "    hos_oot_menu_focus(c, c->x[0], c->x[1], c->x[30]);   /* (nx2/oot_patches.py: the menus' focus) */\n"
    return (source[:at] + hook + source[at:]).replace(
        '#include "a64.h"\n', '#include "a64.h"\nextern void hos_oot_menu_focus(Ctx *c, uint64_t screen, uint64_t pane, uint64_t lr);\n', 1)


MENU_TICK_HEAD = "void f_802f1380(Ctx *__restrict A64_NOESC c) {\n"


def patch_menu_tick(source):
    """f_802f1380, a menu screen's update (each frame, before the screen's UI controller: +56, its vtable's
    +16), calls hos_oot_menu_tick first with the screen: the game's menus' mouse (host/oot_menu.c)"""
    start = source.find(MENU_TICK_HEAD)
    if start < 0:
        return None
    if "hos_oot_menu_tick(c," in source:   # (made before: a lift patched again)
        return source
    at = start + len(MENU_TICK_HEAD)
    hook = "    hos_oot_menu_tick(c, c->x[0]);   /* (nx2/oot_patches.py: the menus' mouse) */\n"
    return (source[:at] + hook + source[at:]).replace(
        '#include "a64.h"\n', '#include "a64.h"\nextern void hos_oot_menu_tick(Ctx *c, uint64_t screen);\n', 1)
