/* (every fixture, force-included by compile(): lib.rs) The settings' accessors (renderer/api/settings.h) for code cut out of
   the runtime: each setting its default, or the value FIXTURE_SET="key=value;key=value" gives (the store's own reader is
   runtime/settings.c, which a fixture does not link). Weak, so a fixture of several files has them once. */
#pragma once
#ifndef _GNU_SOURCE   /* (before any system header: the runtime files a fixture compiles define it first themselves) */
#define _GNU_SOURCE
#endif
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif
static const char *const fixture_setting_keys[SET_COUNT] = {
#define FIXTURE_KEY_(id, key, ...) key,
    OUNCE_SETTINGS(FIXTURE_KEY_, FIXTURE_KEY_, FIXTURE_KEY_, FIXTURE_KEY_, FIXTURE_KEY_)
#undef FIXTURE_KEY_
};
static const double fixture_setting_defs[SET_COUNT] = {
#define FIXTURE_NUM_(id, key, scope, def, ...) (double)(def),
#define FIXTURE_STR_(id, key, scope, def, doc) 0,
    OUNCE_SETTINGS(FIXTURE_NUM_, FIXTURE_NUM_, FIXTURE_NUM_, FIXTURE_NUM_, FIXTURE_STR_)
#undef FIXTURE_NUM_
#undef FIXTURE_STR_
};
/* the value FIXTURE_SET gives the setting, or NULL */
static const char *fixture_setting(int id) {
    static char buf[SET_COUNT][256];
    const char *all = getenv("FIXTURE_SET");
    const size_t n = strlen(fixture_setting_keys[id]);
    for (const char *p = all; p && *p; p = strchr(p, ';') ? strchr(p, ';') + 1 : NULL)
        if (!strncmp(p, fixture_setting_keys[id], n) && p[n] == '=') {
            snprintf(buf[id], sizeof buf[id], "%.*s", (int)strcspn(p + n + 1, ";"), p + n + 1);
            return buf[id];
        }
    return NULL;
}
/* a fixture's own answers, set while it runs (1: *v is the setting's value) */
static int (*fixture_setting_hook)(setting_id id, int64_t *v);
__attribute__((weak)) int64_t setting_int(setting_id id) {
    int64_t h;
    if (fixture_setting_hook && fixture_setting_hook(id, &h)) return h;
    const char *v = fixture_setting(id);
    return v ? (int64_t)strtoll(v, NULL, 0) : (int64_t)fixture_setting_defs[id];
}
__attribute__((weak)) int setting_bool(setting_id id) { return setting_int(id) != 0; }
__attribute__((weak)) double setting_float(setting_id id) {
    const char *v = fixture_setting(id);
    return v ? strtod(v, NULL) : fixture_setting_defs[id];
}
__attribute__((weak)) const char *setting_str(setting_id id) { const char *v = fixture_setting(id); return v ? v : ""; }
__attribute__((weak)) int setting_given(setting_id id) { return fixture_setting(id) != NULL; }
__attribute__((weak)) void setting_write_int(setting_id id, int64_t v) { (void)id, (void)v; }
__attribute__((weak)) void setting_write_float(setting_id id, double v) { (void)id, (void)v; }
#ifdef __cplusplus
}
#endif
