#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MIB (UINT64_C(1) << 20)
volatile uint64_t hos_wide_out;
static unsigned output_width, output_height;
static const char *override;
static uint32_t hos_output_size(int height) { return height ? output_height : output_width; }
static void hos_log(const char *fmt, ...) { (void)fmt; }
static const char *fixture_env(const char *name) {
    if (strcmp(name, "HOS_RT_POOL_MB")) exit(2);
    return override;
}
#define getenv fixture_env
@BODY@
#define WIDE(w, h) ((uint64_t)(h) << 32 | (w))
static const struct {
    unsigned width, height;
    uint64_t wide, game_mib;
    const char *env;
    uint64_t expected_mib;
} cases[] = {
    {2560, 1440, 0, 305, NULL, 512},
    {3840, 2160, 0, 305, NULL, 687},
    {2560, 1440, WIDE(3440, 1440), 305, NULL, 512},
    {2560, 1440, WIDE(5120, 1440), 305, NULL, 610},
    {3840, 2160, WIDE(7680, 2160), 305, NULL, 687},   /* (a larger picture is only room: 3840 x 2160 at most) */
    {2560, 1440, WIDE(1920, 1080), 305, NULL, 512},
    {2560, 1440, 0, 305, "256", 256},
    {3840, 2160, WIDE(7680, 2160), 305, "768", 768},
    {2560, 1440, 0, 305, "0x200", 512},
    {2560, 1440, 0, 305, "0", 0},
    {2560, 1440, 0, 768, NULL, 768},
    {3840, 2160, 0, 768, NULL, 1728},
};
int main(void) {
    int failed = 0;
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        output_width = cases[i].width; output_height = cases[i].height;
        hos_wide_out = cases[i].wide; override = cases[i].env;
        uint64_t got = hos_rt_pool_size(cases[i].game_mib * MIB);
        if (got != cases[i].expected_mib * MIB) {
            fprintf(stderr, "case %u: got %llu bytes, expected %llu MiB\n", i,
                    (unsigned long long)got, (unsigned long long)cases[i].expected_mib);
            failed = 1;
        }
    }
    return failed;
}
