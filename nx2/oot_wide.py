"""Widescreen and ultrawide for Ocarina of Time 1.1.1. The picture and scene sizes, the camera ratios and the movie viewport
are lift 2 At probes (nx2/lift2/probes.py AT, host/oot_probes.c hos_oot_wide_*); the HUD's projection and the save's
picture stay C after an instruction on the whole context (Discovery.after, probes.after_c): the HUD's reads and
rewrites two Q registers, the save picture's four W registers at once, which an At row (one result) cannot.

The game draws a 2560x1440 picture whatever the screen and renders its scene at 1920x1080; its cameras use a 16:9 ratio.
With these patches the picture takes the window's shape (the height stays the game's, the width grows on an ultrawide
screen and shrinks on a 16:10 or 4:3 one), the 16:9 cameras take the picture's ratio, the HUD and menus keep their 16:9
shape in the middle of a wider picture (a narrower one: its whole width), pre-rendered movies play in that middle 16:9,
and the picture a save keeps shows that middle 16:9 (a narrower picture: its middle band, at the texture's shape). Every patch reads host globals
(host/hos_wide.c) that stay 0 for a 16:9 window, so the game runs as shipped then.

The sites and what each does come from NX2navi's recomp/patches-v111.txt, where they are credited to Ivan0xFF (MIT,
"The matrix-update, UI and movie sites: Copyright (c) 2026 Ivan0xFF"); the picture and scene size sites are the same
constants that file names; the save picture's site is Hookshot's own (issue 952). Each site's original instruction word is checked: a word that differs stops the lift.
"""

MAIN = 0x80020000   # (main's base in this image: nx2/eden_image.py)
UI_M00, UI_M11 = "(2.0f / 3840.0f)", "(2.0f / 2160.0f)"   # (the UI's full-screen orthographic projection, 3840x2160 grid)
# (the screens whose UI keeps the whole picture: the item-wheel and pause / system menu backgrounds redraw a capture of
# the whole picture; the event and loading fades cover it)
KEEP_VTABLES = (MAIN + 0x4bed098, MAIN + 0x4be4fb0, MAIN + 0x4be61c8, MAIN + 0x4be7b30)


HUD = ("{ extern volatile float hos_wide_squeeze; const float _q = hos_wide_squeeze; if (_q > 0.0f) { float _m00, _m11; "
       "memcpy(&_m00, c->vreg[1], 4); memcpy(&_m11, (const uint8_t *)c->vreg[4] + 4, 4); "
       "if (_m00 == %s && _m11 == %s) { const uint64_t _vt = recomp_load64(c, c->x[0] - 0x1b8); "
       "if (%s) { float _row[4]; memcpy(_row, c->vreg[1], 16); for (int _k = 0; _k < 4; _k++) _row[_k] *= _q; "
       "memcpy(c->vreg[1], _row, 16); } } } }   /* (nx2/oot_wide.py: HUD in the middle 16:9) */"
       % (UI_M00, UI_M11, " && ".join("_vt != 0x%xULL" % v for v in KEEP_VTABLES)))

# The save's picture (issue 952): the pause menu draws the whole picture into a 543 x 306 texture, the save's JPEG, through the
# game's viewport-and-scissor helper (f_802a46e0: SetScissor, then SetViewport(x, y, w, h) in w1-w4 from [x8], called by blr x8).
# Wider than 16:9, the whole picture was squeezed into it. That one viewport is made wider than the texture by the picture's
# ratio over 16:9, centred: the middle 16:9 fills the texture (the scissor keeps the texture's), as a 16:9 window would show.
# Narrower than 16:9 (4:3, 16:10) it is made taller instead, by 16:9 over the picture's ratio: the picture's middle band fills
# the texture at its own shape, neither squeezed nor with bars.
THUMB = ("{ extern volatile float hos_wide_ratio; const float _r = hos_wide_ratio; if (_r > 0.0f && (uint32_t)c->x[1] == 0 "
         "&& (uint32_t)c->x[2] == 0 && (uint32_t)c->x[3] == 543 && (uint32_t)c->x[4] == 306) { "
         "if (_r > 16.0f / 9.0f) { const int _w = (int)(543.0f * _r / (16.0f / 9.0f) + 0.5f); "
         "c->x[1] = (uint32_t)((543 - _w) / 2); c->x[3] = (uint32_t)_w; } "
         "else { const int _h = (int)(306.0f * (16.0f / 9.0f) / _r + 0.5f); "
         "c->x[2] = (uint32_t)((306 - _h) / 2); c->x[4] = (uint32_t)_h; } } }"
         "   /* (nx2/oot_wide.py: the save's picture, the middle 16:9) */")

# (main offset, the original instruction word there, the C after it)
SITES = (
    # the 2D canvas setup: the layout's projection rows (ldp q1, q4, [x2]); the canvas is x0, its screen 0x1b8 before it
    (0x774368, 0xad401041, HUD),
    # the viewport helper's SetViewport, after the call's address is loaded (ldr x8, [x8]), before blr x8
    (0x284824, 0xf9400108, THUMB),
)


def check(img, name, sites):
    """SystemExit unless every site holds its word (another game version, or a site that moved)"""
    for off, word in sites:
        try:
            have = img.word(MAIN + off)
        except Exception:
            have = None
        if have != word:
            raise SystemExit("%s: main+%#x holds %s, not %08x: the game's code is not what the patch expects" % (
                name, off, "nothing" if have is None else "%08x" % have, word))


def install(lifter, img):
    """The sites into lifter.after; SystemExit when a site's word differs"""
    check(img, "widescreen", [(off, word) for off, word, _ in SITES])
    for off, word, text in SITES:
        lifter.after[MAIN + off] = text
    print("widescreen: %d sites" % len(SITES))
    return len(SITES)
