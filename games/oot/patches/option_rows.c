/* Game patch: OunceRecomp's own settings as rows of the game's Options screen, with no game file changed.

   The screen (UISystemOptionScreen, the pause menu's and the title's Options) makes one row per entry of the option
   table, the parsed UI/OptionSetTable bgyml that hangs off the game's UI singleton (+0x500: a count and an array of
   0x28-byte entries), cloning a template part of its layout by the entry's ButtonType. Before it builds its rows
   (ui_option_screen_build_rows), the table here is the game's entries followed by a separator and one left/right
   choice row per setting, in guest memory of ours; the game then makes, places, focuses and scrolls them itself.

   Our rows have OptionType 18 and up (one per setting; the game's run 0..17). The game's lookups by OptionType are
   bounds-checked (an empty name above 17), its sequence takes the selected type as a number that none of its branches
   asks for, and an entry's value key is 0, which the game data reader and writer refuse: nothing of ours enters
   OptionData or the game's option save. What the game reads from its message file for a row, the title, the
   choice's text and the help line, is set here instead from texts in guest memory of ours (a text pane keeps the
   pointer it is given). The choice's index (+0x5c) is ours: put in before the row's setup, which then sets its arrows
   by it, and taken whenever the row shows a new one (each step and decide ends in ui_option_choice_row_set_text).

   "Default Settings" (Y) resets the game's own options and leaves ours: they are the host's settings, kept in the options
   file, not part of the game's option save.

   Check that it applies: the line "settings: <n> rows of ours after the game's <m>" when the screen first opens, and
   "settings: <key> = <choice>, kept in <file>" at each change. */
#include "patches.h"
#include <stddef.h>
#include <string.h>
#include "os.h"

#define UI_SINGLETON_REF 0x84c9e2a8ull   /* (holds the address of the UI singleton's pointer: ui_option_screen_build_rows) */
#define SET_BOUND_TEXT 0x80a87d30ull     /* ui_layout_set_bound_text(layout, name, ref, 1, 0) */
#define STR_T_OPTION 0x83dc7779ull       /* "T_Option_00": a choice row's title */
#define STR_T_TEXT 0x83de2306ull         /* "T_Text_00": its choice */
#define STR_T_CAPTION 0x83e3e351ull      /* "T_Caption_00": the screen's help line */
enum { TYPE_OURS = 18, BUTTON_CHOICE = 1, BUTTON_SEPARATOR = 3 };

typedef struct __attribute__((may_alias)) {
    int32_t count;
    uint32_t _4;
    uint64_t rows;
    uint32_t focused_type;   /* (+0x10: the OptionType focused last) */
} OptionTable;
typedef struct __attribute__((may_alias)) {
    uint32_t type;      /* OptionType */
    uint32_t button;    /* ButtonType: 0 toggle, 1 choice, 2 button, 3 separator */
    uint32_t _8, _c;
    uint32_t choices;   /* +0x10: the choices' count (the arrows' ends) */
    uint32_t _14;
    uint64_t choice;    /* +0x18: their enum values (only the game data writer reads them) */
    uint32_t key;       /* +0x20: the value's game data key (0: none) */
    uint32_t check;     /* +0x24: the game data bool that shows the row (0: always) */
} OptionRow;
_Static_assert(sizeof(OptionRow) == 0x28 && offsetof(OptionRow, choices) == 0x10 && offsetof(OptionRow, choice) == 0x18 &&
                   offsetof(OptionRow, key) == 0x20 && offsetof(OptionRow, check) == 0x24, "the option table's entry");
/* a choice row object (ui_option_choice_row_setup): its part layout, its OptionType, the index shown */
typedef struct __attribute__((may_alias)) {
    uint8_t _0[0x20];
    uint64_t layout;   /* +0x20 */
    uint8_t _28[0x30];
    uint32_t type;     /* +0x58 */
    int32_t index;     /* +0x5c */
} ChoiceRow;
_Static_assert(offsetof(ChoiceRow, layout) == 0x20 && offsetof(ChoiceRow, type) == 0x58 && offsetof(ChoiceRow, index) == 0x5c,
               "the choice row");

/* The rows: an enum setting each (renderer/api/settings.h), its choices in the setting's order. A render resolution row
   waits for the host upscaler: below 100% the 3D is drawn into a corner of the frame and nothing scales it up */
enum { MAX_CHOICES = 6 };
typedef struct {
    setting_id id;
    const char *title, *caption;
    int count;
    const char *choice[MAX_CHOICES];
} Setting;
static const Setting settings[] = {
    {SET_FRAME_RATE, "Frame Rate", "Sets the frame rate cap.\nVSync matches the display's refresh rate.", 4, {"30", "60", "120", "VSync"}},
};
enum { N = sizeof settings / sizeof *settings };

/* a text in guest memory of ours: kept for as long as the game runs, as the panes keep the pointer */
typedef struct { uint64_t at; uint32_t bytes; } Text;
static struct { Text title, caption, choice[MAX_CHOICES]; } texts[N];
static uint64_t our_rows;   /* (the table's entries while they are ours) */

static Text text(const char *s) {
    const size_t n = strlen(s);
    const uint64_t at = hos_sys_alloc(n + 1, 8);
    memcpy(GP(at), s, n + 1);
    return (Text){at, (uint32_t)n};
}

/* the ours of a row's OptionType, -1 for the game's */
static int ours(uint32_t type) { return type >= TYPE_OURS && type < TYPE_OURS + N ? (int)(type - TYPE_OURS) : -1; }

/* sets one bound text pane of a layout to t: a message reference {text, bytes, attribute -1} on the guest stack
   under sp, the call on the stack under it */
static void set_text(GuestCall *g, uint64_t sp, uint64_t layout, uint64_t name, Text t) {
    const uint64_t ref = (sp - 0x20) & ~15ull;
    G64(ref) = t.at, G64(ref + 8) = t.bytes, G64(ref + 16) = ~0ull;
    guest_call(g, ref - 0x20, SET_BOUND_TEXT, layout, name, ref, 1, 0);
}

/* ui_option_screen_build_rows: the table is the game's entries, a separator, then ours */
PATCH_AT(0x834211c0, word(0xd503237f), before, none, settings_rows) {   /* pacibsp */
    static os_mutex lock = OS_MUTEX_INIT;
    os_mutex_lock(&lock);
    OptionTable *t = (OptionTable *)GP(G64(G64(G64(UI_SINGLETON_REF)) + 0x500));
    if (t->rows != our_rows) {
        const OptionRow *game = (const OptionRow *)GP(t->rows);
        const OptionRow *separator = NULL, *choice = NULL;
        for (int i = 0; i < t->count; i++) {
            if (game[i].button == BUTTON_SEPARATOR && !separator) separator = &game[i];
            if (game[i].button == BUTTON_CHOICE && !choice) choice = &game[i];
            if (ours(game[i].type) >= 0) hos_fatal(NULL, "settings: the game's option table has OptionType %u", game[i].type);
        }
        if (!separator || !choice) hos_fatal(NULL, "settings: the game's option table has no separator or no choice row");
        const int n = t->count + 1 + N;
        const uint64_t rows = hos_sys_alloc(n * sizeof(OptionRow), 8);
        OptionRow *r = (OptionRow *)GP(rows);
        memcpy(r, game, t->count * sizeof(OptionRow));
        r[t->count] = *separator;
        r[t->count].check = 0;
        for (int i = 0; i < N; i++) {
            OptionRow *o = &r[t->count + 1 + i];
            *o = *choice;
            o->type = TYPE_OURS + i, o->key = 0, o->check = 0;
            o->choices = settings[i].count;
            o->choice = hos_sys_alloc(MAX_CHOICES * 4, 8);   /* (zeros: the writer, refused at key 0, is all that reads them) */
            if (!texts[i].title.at) {
                texts[i].title = text(settings[i].title), texts[i].caption = text(settings[i].caption);
                for (int k = 0; k < settings[i].count; k++) texts[i].choice[k] = text(settings[i].choice[k]);
            }
        }
        hos_log("settings: %d rows of ours after the game's %d", N + 1, t->count);
        our_rows = rows;
        __atomic_store_n(&t->rows, rows, __ATOMIC_RELEASE);   /* (the entries first: the game's own come first in ours) */
        __atomic_store_n(&t->count, n, __ATOMIC_RELEASE);
    }
    os_mutex_unlock(&lock);
}

/* ui_option_choice_row_setup(row, entry, flag): ours gets its index before the setup and its title after */
PATCH_FUNC(0x833ef590, word(0xd503237f), body(0xcd0005f327864f67), aapcs, none, settings_choice_setup, orig,
           guest(0x80a87d30), sp, as(x0, row), as(x1, entry), as(x2, flag)) {
    ChoiceRow *r = (ChoiceRow *)GP(row);
    const int i = ours(((const OptionRow *)GP(entry))->type);
    if (i >= 0) r->index = (int32_t)setting_int(settings[i].id);
    guest_orig(orig, row, entry, flag);
    if (i >= 0) set_text(g, sp, r->layout, STR_T_OPTION, texts[i].title);
}

/* ui_option_choice_row_set_text(row): ours shows its choice, and a new index is the setting's new value (written to the
   options file: runtime/settings.c) */
PATCH_FUNC(0x833ef7f0, word(0xd503237f), body(0x2e2f49591b6a8d04), aapcs, none, settings_choice_text, orig,
           guest(0x80a87d30), sp, as(x0, row)) {
    const ChoiceRow *r = (const ChoiceRow *)GP(row);
    const int i = ours(r->type);
    if (i < 0) {
        guest_orig(orig, row);
        return;
    }
    const Setting *s = &settings[i];
    if (r->index < 0 || r->index >= s->count) hos_fatal(NULL, "settings: %s's row shows choice %d", s->title, r->index);
    if (setting_int(s->id) != r->index) setting_write_int(s->id, r->index);
    set_text(g, sp, r->layout, STR_T_TEXT, texts[i].choice[r->index]);
}

/* ui_option_screen_set_caption(screen, type): the help line of ours */
PATCH_FUNC(0x834231d0, word(0xd503237f), body(0xfcd6422451b87fe6), aapcs, none, settings_caption, orig, guest(0x80a87d30),
           sp, as(x0, screen), as(w1, type)) {
    const int i = ours(type);
    if (i < 0) {
        guest_orig(orig, screen, type);
        return;
    }
    set_text(g, sp, G64(screen + 0x28), STR_T_CAPTION, texts[i].caption);
}
