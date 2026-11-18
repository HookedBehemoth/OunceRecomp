#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile uint64_t hos_wide_out, hos_wide_scene;
volatile float hos_wide_ratio, hos_wide_squeeze, hos_wide_margin;
float hos_aspect_ratio;
static unsigned out_h = 1440;
uint32_t hos_output_size(int height) { return height ? out_h : out_h * 16 / 9; }
static void hos_log(const char *f, ...) { (void)f; }
int hos_output_res_set(void) { return 1; }   /* (an Output resolution chosen: the fixed-size branch) */
void game_frame_size(uint32_t w, uint32_t h, int start) { (void)w, (void)h, (void)start; }

static void wide(float want) {
    const char *e = "1";
    const int dw = 0, dh = 0, ww = 0, wh = 0;
@SIZES@
}

int main(int argc, char **argv) {
    out_h = (unsigned)atoi(argv[2]);
    wide((float)atof(argv[1]));
    printf("%llux%llu %llux%llu %.4f %.4f %.4f\n",
           (unsigned long long)(hos_wide_out & 0xffffffffu), (unsigned long long)(hos_wide_out >> 32),
           (unsigned long long)(hos_wide_scene & 0xffffffffu), (unsigned long long)(hos_wide_scene >> 32),
           (double)hos_wide_ratio, (double)hos_wide_squeeze, (double)hos_wide_margin);
    return 0;
}
