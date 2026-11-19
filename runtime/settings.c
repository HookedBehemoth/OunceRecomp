/* The settings store: the one reader and writer of renderer/api/settings.h's keys.

   At the start (settings_init, before anything asks): the defaults, then the options file's user settings, then the
   command line's --set key=value (any setting, for this run only). The options file is --options <file>, else
   ounce_options.ini beside the game. A line of the file that is not a user setting, or not a value the setting takes,
   is reported and left out; a --set that is not stops the start. A user setting changed while the game runs
   (setting_write_*) is written back into the file, its other lines kept; a --set value is never written. */
#include "settings.h"
#include "hos.h"
#include "hos_paths.h"
#include "os.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { T_BOOL, T_INT, T_FLOAT, T_ENUM, T_STRING };
enum { DEV, USER };
typedef struct {
    const char *key, *choices, *def_s, *doc;
    unsigned char type, user;
    double def, lo, hi;
} info_t;
static const info_t info[SET_COUNT] = {
#define B_(id, key, scope, def, doc) {key, NULL, NULL, doc, T_BOOL, scope, def, 0, 1},
#define I_(id, key, scope, def, lo, hi, doc) {key, NULL, NULL, doc, T_INT, scope, def, (double)(lo), (double)(hi)},
#define F_(id, key, scope, def, lo, hi, doc) {key, NULL, NULL, doc, T_FLOAT, scope, def, lo, hi},
#define E_(id, key, scope, def, choices, doc) {key, choices, NULL, doc, T_ENUM, scope, def, 0, 0},
#define S_(id, key, scope, def, doc) {key, NULL, def, doc, T_STRING, scope, 0, 0, 0},
    OUNCE_SETTINGS(B_, I_, F_, E_, S_)
#undef B_
#undef I_
#undef F_
#undef E_
#undef S_
};

/* the values: numbers as a double's bits (an int64 fits a double up to 2^53, more than any setting's range) */
static volatile uint64_t num[SET_COUNT];
static const char *str[SET_COUNT];
static unsigned char given[SET_COUNT], from_file[SET_COUNT];
static char path[OS_PATH_MAX];
static os_mutex write_lock = OS_MUTEX_INIT;
static char notes[4096];   /* (what the start found, logged by settings_report once the log is open) */
static size_t notes_n;
static int ready;

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(notes + notes_n, sizeof notes - notes_n, fmt, ap);
    va_end(ap);
    if (n > 0 && notes_n + (size_t)n + 1 < sizeof notes) notes_n += (size_t)n, notes[notes_n++] = '\n';
}

static uint64_t bits(double v) { uint64_t u; memcpy(&u, &v, 8); return u; }
static double value(setting_id id) { const uint64_t u = __atomic_load_n(&num[id], __ATOMIC_RELAXED); double v; memcpy(&v, &u, 8); return v; }

static int find(const char *key, size_t n) {
    for (int i = 0; i < SET_COUNT; i++)
        if (strlen(info[i].key) == n && !strncmp(info[i].key, key, n)) return i;
    return -1;
}
/* the index of a choice by its name (any case) or its number; -1 none */
static int choice(const info_t *s, const char *v) {
    int i = 0;
    for (const char *c = s->choices; *c; i++) {
        const char *e = strchr(c, '|');
        const size_t n = e ? (size_t)(e - c) : strlen(c);
        if (strlen(v) == n && !strncasecmp(c, v, n)) return i;
        c = e ? e + 1 : c + n;
    }
    char *end;
    const long k = strtol(v, &end, 10);
    return *v && !*end && k >= 0 && k < i ? (int)k : -1;
}
static const char *choice_name(const info_t *s, int k, char *out, size_t cap) {
    const char *c = s->choices;
    for (int i = 0; i < k && c; i++) c = strchr(c, '|') ? strchr(c, '|') + 1 : NULL;
    snprintf(out, cap, "%.*s", c ? (int)strcspn(c, "|") : 0, c ? c : "");
    return out;
}

/* parses v into setting i; 0 when it is not a value the setting takes (why in err) */
static int parse(int i, const char *v, char *err, size_t cap) {
    const info_t *s = &info[i];
    char *end;
    switch (s->type) {
    case T_BOOL: {
        const int on = !strcmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "on") || !strcasecmp(v, "yes");
        const int off = !strcmp(v, "0") || !strcasecmp(v, "false") || !strcasecmp(v, "off") || !strcasecmp(v, "no");
        if (!on && !off) return snprintf(err, cap, "%s takes 1 or 0, not \"%s\"", s->key, v), 0;
        num[i] = bits(on);
        return 1;
    }
    case T_INT: {
        const long long k = strtoll(v, &end, 0);
        if (!*v || *end || k < s->lo || k > s->hi)
            return snprintf(err, cap, "%s takes a whole number from %.0f to %.0f, not \"%s\"", s->key, s->lo, s->hi, v), 0;
        num[i] = bits((double)k);
        return 1;
    }
    case T_FLOAT: {
        const double f = strtod(v, &end);
        if (!*v || *end || !(f >= s->lo && f <= s->hi))
            return snprintf(err, cap, "%s takes a number from %g to %g, not \"%s\"", s->key, s->lo, s->hi, v), 0;
        num[i] = bits(f);
        return 1;
    }
    case T_ENUM: {
        const int k = choice(s, v);
        if (k < 0) return snprintf(err, cap, "%s takes one of %s, not \"%s\"", s->key, s->choices, v), 0;
        num[i] = bits(k);
        return 1;
    }
    default:
        str[i] = strdup(v);
        num[i] = bits(*v != 0);   /* (a string given is "on": dbg() in the renderer) */
        return 1;
    }
}

static char *trim(char *s) {
    if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s += 3;   /* (a BOM) */
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && strchr(" \t\r\n", s[n - 1])) s[--n] = 0;
    return s;
}

static void read_file(void) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512], err[256];
    int lines = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line), *eq = strchr(s, '=');
        if (!*s || *s == '#' || *s == ';' || *s == '[') continue;
        lines++;
        const int i = eq ? find(s, (size_t)(eq - s)) : -1;
        if (i < 0 || !info[i].user) {
            note("settings: %s: \"%s\" is not a user setting: left out", path, s);
            continue;
        }
        if (!parse(i, trim(eq + 1), err, sizeof err)) note("settings: %s: %s: the default kept", path, err);
        else given[i] = from_file[i] = 1;
    }
    fclose(f);
    note("settings: %s: %d lines", path, lines);
}

static void defaults(void) {
    for (int i = 0; i < SET_COUNT; i++) num[i] = bits(info[i].def), str[i] = info[i].def_s ? info[i].def_s : "";
}

/* --list-settings: every setting, its value now (after the options file and --set), its default, range and use */
static void print_table(void) {
    static const char *const type[] = {"bool", "int", "float", "enum", "string"};
    char d[64], v[64];
    for (int i = 0; i < SET_COUNT; i++) {
        const info_t *s = &info[i];
        if (s->type == T_STRING) snprintf(d, sizeof d, "\"%s\"", s->def_s);
        else if (s->type == T_ENUM) choice_name(s, (int)s->def, d, sizeof d);
        else snprintf(d, sizeof d, "%g", s->def);
        setting_text((setting_id)i, v, sizeof v);
        printf("%-28s %-4s %-6s %-12s default %-10s", s->key, s->user ? "user" : "dev", type[s->type], v, d);
        if (s->type == T_ENUM) printf(" (%s)", s->choices);
        else if (s->type == T_INT || s->type == T_FLOAT) printf(" (%g to %g)", s->lo, s->hi);
        printf("  %s\n", s->doc);
    }
}

int settings_init(int argc, char **argv) {
    defaults();
    const char *file = NULL;
    int n = 1;
    for (int i = 1; i < argc; i++) {   /* (--options first: --set goes over the file's values, wherever it stands) */
        if (!strcmp(argv[i], "--options") && i + 1 < argc) file = argv[++i];
    }
    if (file) snprintf(path, sizeof path, "%s", file);
    else {
        uint32_t k = hos_game_path_a(path, OS_PATH_MAX);
        while (k && path[k - 1] != '/') k--;
        snprintf(path + k, sizeof path - k, "ounce_options.ini");
    }
    read_file();
    char err[256];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--options") && i + 1 < argc) { i++; continue; }
        if (strcmp(argv[i], "--set") || i + 1 >= argc) { argv[n++] = argv[i]; continue; }
        const char *kv = argv[++i], *eq = strchr(kv, '=');
        const int k = eq ? find(kv, (size_t)(eq - kv)) : -1;
        if (k < 0) {
            fprintf(stderr, "--set %s: no such setting (--list-settings lists them)\n", kv);
            exit(2);
        }
        if (!parse(k, eq + 1, err, sizeof err)) {
            fprintf(stderr, "--set: %s\n", err);
            exit(2);
        }
        given[k] = 1, from_file[k] = 0;
    }
    argv[n] = NULL;
    ready = 1;
    for (int i = 1; i < n; i++)
        if (!strcmp(argv[i], "--list-settings")) print_table(), exit(0);
    return n;
}

void settings_report(void) {
    if (notes_n) hos_log("%.*s", (int)notes_n - 1, notes);
    char v[64];
    for (int i = 0; i < SET_COUNT; i++)
        if (given[i]) hos_log("settings: %s = %s (%s)", info[i].key, info[i].type == T_STRING ? str[i] : setting_text((setting_id)i, v, sizeof v),
                              from_file[i] ? "the options file" : "--set");
}

static const info_t *at(setting_id id) {
    if ((unsigned)id >= SET_COUNT) hos_fatal(NULL, "settings: no setting %d", (int)id);
    if (!ready) defaults(), ready = 1;   /* (asked before main: a test's fixture) */
    return &info[id];
}
int setting_bool(setting_id id) { at(id); return value(id) != 0; }
int64_t setting_int(setting_id id) { at(id); return (int64_t)value(id); }
double setting_float(setting_id id) { at(id); return value(id); }
const char *setting_str(setting_id id) {
    if (at(id)->type != T_STRING) hos_fatal(NULL, "settings: %s is not a string", info[id].key);
    return str[id];
}
int setting_given(setting_id id) { at(id); return given[id]; }
const char *setting_text(setting_id id, char *out, size_t cap) {
    const info_t *s = at(id);
    if (s->type == T_STRING) snprintf(out, cap, "%s", str[id]);
    else if (s->type == T_ENUM) choice_name(s, (int)value(id), out, cap);
    else snprintf(out, cap, "%g", value(id));
    return out;
}

/* the file again with key's line set to its value (or added), the other lines as they were; through a temporary file */
static void write_back(setting_id id) {
    char text[64], tmp[OS_PATH_MAX + 8], line[512];
    setting_text(id, text, sizeof text);
    const info_t *s = &info[id];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *in = fopen(path, "r"), *out = fopen(tmp, "w");
    if (!out) {
        hos_log("settings: %s cannot be written: %s = %s not kept", tmp, s->key, text);
        if (in) fclose(in);
        return;
    }
    int done = 0;
    const size_t n = strlen(s->key);
    while (in && fgets(line, sizeof line, in)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, s->key, n) && (p[n] == '=' || p[n] == ' ')) {
            if (!done) fprintf(out, "%s=%s\n", s->key, text);
            done = 1;
        } else {
            fputs(line, out);
        }
    }
    if (!done) fprintf(out, "%s=%s\n", s->key, text);
    if (in) fclose(in);
    fclose(out);
    if (os_rename(tmp, path, true)) hos_log("settings: %s cannot be replaced: %s = %s not kept", path, s->key, text);
    else hos_log("settings: %s = %s, kept in %s", s->key, text, path);
}
static void write_num(setting_id id, double v) {
    const info_t *s = at(id);
    if (!s->user) hos_fatal(NULL, "settings: %s is a dev setting: it is not changed while the game runs", s->key);
    if (s->type == T_STRING) hos_fatal(NULL, "settings: %s is a string", s->key);
    if (s->type == T_ENUM) {
        char c[64];
        if (v < 0 || !*choice_name(s, (int)v, c, sizeof c)) hos_fatal(NULL, "settings: %s has no choice %g", s->key, v);
    } else if (s->type != T_BOOL && !(v >= s->lo && v <= s->hi)) {
        hos_fatal(NULL, "settings: %s = %g is outside %g to %g", s->key, v, s->lo, s->hi);
    }
    os_mutex_lock(&write_lock);
    __atomic_store_n(&num[id], bits(v), __ATOMIC_RELAXED);
    given[id] = from_file[id] = 1;
    write_back(id);
    os_mutex_unlock(&write_lock);
}
void setting_write_int(setting_id id, int64_t v) { write_num(id, (double)v); }
void setting_write_float(setting_id id, double v) { write_num(id, v); }
