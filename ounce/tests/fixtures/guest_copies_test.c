
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "hos.h"
uint8_t *window;
void hos_fatal(Ctx *c, const char *fmt, ...) {   /* (the stop: its message, exit 7) */
    (void)c;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    _exit(7);
}
#include "nn_libc.c"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
int main(int argc, char **argv) {
    window = mmap(NULL, WINDOW + 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(window != MAP_FAILED);
    memset(window + WINDOW, 0xAB, 0x1000);
    Ctx c = {{0}};
    c.x[30] = 0x80001234;
    const int stop = argc > 1 ? atoi(argv[1]) : 0;
    if (stop) {   /* (a range outside the window: a fatal stop, the page after it untouched) */
        static const uint64_t cases[][3] = {{WINDOW - 8, 0, 64}, {WINDOW - 8, 0x100, 64}, {0x100, WINDOW + 0x10, 8},
                                            {~0ull - 4, 0, 16}};
        c.x[0] = cases[stop - 1][0], c.x[1] = cases[stop - 1][1], c.x[2] = cases[stop - 1][2];
        if (stop == 1 || stop == 4) hos_func_sym_memset(&c);
        else if (stop == 2) hos_func_sym_memmove(&c);
        else hos_func_sym_memcpy(&c);
        return 0;   /* (not reached) */
    }
    /* inside the window: the host copies, x0 the destination */
    memcpy(window + 0x100, "guest", 6);
    c.x[0] = 0x200, c.x[1] = 0x100, c.x[2] = 6;
    hos_func_sym_memcpy(&c);
    CHECK(c.x[0] == 0x200 && !memcmp(window + 0x200, "guest", 6));
    c.x[0] = 0x300, c.x[1] = 0x41, c.x[2] = 16;
    hos_func_sym_memset(&c);
    CHECK(c.x[0] == 0x300 && window[0x30f] == 0x41);
    c.x[0] = ~0ull - 4, c.x[1] = 0, c.x[2] = 0;   /* (nothing to copy: no range to check) */
    hos_func_sym_memcpy(&c);
    CHECK(c.x[0] == ~0ull - 4);
    printf("PASS: the libc replacements copy inside the guest window\n");
    return 0;
}
