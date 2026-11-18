#include <stdio.h>
#include <string.h>
static struct { unsigned rested; signed char held[4]; } pads[1];
static void step(int i, int v[4]) {
@BLOCK@
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
int main(void) {
    int v[4];
    /* a phantom: axis 2 held at -32767 from the start, never at rest */
    memset(pads, 0, sizeof pads);
    for (int n = 0; n < 100; n++) {
        int w[4] = {0, 0, -32767, 0};
        step(0, w);
        CHECK(w[2] == 0);
    }
    /* the other axes are free: axis 0 seen at rest, then held */
    { int w[4] = {20000, 0, -32767, 0}; step(0, w); CHECK(w[0] == 20000 && w[2] == 0); }
    /* a stick at rest at the start steers either way */
    memset(pads, 0, sizeof pads);
    { int w[4] = {0, 0, 0, 0}; step(0, w); }
    { int w[4] = {-30000, 12000, 0, 0}; step(0, w); CHECK(w[0] == -30000 && w[1] == 12000); }
    /* a worn stick resting past the dead zone (held +9000 from the start): not until pushed the other way */
    memset(pads, 0, sizeof pads);
    for (int n = 0; n < 10; n++) { int w[4] = {9000, 0, 0, 0}; step(0, w); CHECK(w[0] == 0); }
    { int w[4] = {-25000, 0, 0, 0}; step(0, w); CHECK(w[0] == -25000); }
    { int w[4] = {9000, 0, 0, 0}; step(0, w); CHECK(w[0] == 9000); }
    (void)v;
    printf("PASS: stick axes steer once seen at rest; a held phantom axis never does\n");
    return 0;
}
