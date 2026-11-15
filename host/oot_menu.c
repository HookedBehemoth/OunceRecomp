/* The mouse in the game's own menus: the title and its lists, the pause menu, the panel of
   Key Items, Threads of Time and the Map, the options, the save files' dialogs. The mouse frees itself over them
   (display.c), points at their buttons (the focus as the pad moves it), clicks them (A), backs out (the right button:
   B) and scrolls their lists. nx2/oot_parked.py had two of the game's functions call here first (parked while the present
   lifter is retired: the game's menus do not follow the mouse until the hooks are ported):
   - f_802ed660 (in the pause menu's vtable at 0x84c0bc18), the pause menu's handler of a pane getting the focus: it
     matches the pane's name against the pages (PaItemMenu_00, PaEquipmentMenu_00, PaCollectMenu_00, PaSystemMenu_00)
     and hands the pane to the page (f_802ebd20 on the Items page: the info panel). The pause menu's page, for its tabs.
   - f_802f1380, every menu screen's update (each frame, before its UI controller at +56 runs the buttons' requests):
     the screens are tracked, and the mouse is for the one in front (hos_oot_menu_tick).
   The focus setter is f_8088ffc0(screen, button, flag), as the game's screens call it (f_80795090): it finds the button's
   entry in the screen's list (+136 the head, +144 the first, +156 the entry's offset from its node; entry+24 the button)
   and hands it to the focus manager (*(screen+32)+96, its vtable +48), when the manager takes the player's input.
   HOS_MENU_TRACE=1: the focus changes, the screens and what the mouse does logged. */
#include "hos.h"
void hos_oot_menu_focus(Ctx *c, uint64_t screen, uint64_t pane, uint64_t lr) { (void)c, (void)screen, (void)pane, (void)lr; }
void hos_oot_menu_tick(Ctx *c, uint64_t screen) { (void)c, (void)screen; }
