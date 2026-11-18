#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/sysinfo.h>
static int boxes;
static void hos_log(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    printf("log: "), vprintf(fmt, a), printf("\n");
    va_end(a);
}
static void hos_sdl_message_box(const char *title, const char *text) {
    (void)title, (void)text;
    ++boxes;
}
static int os_mem_available(uint64_t *ram, uint64_t *swap) {   /* (runtime/os/os_linux.c's) */
    struct sysinfo si;
    if (sysinfo(&si)) return -1;
    *ram = (uint64_t)si.totalram * si.mem_unit, *swap = (uint64_t)si.totalswap * si.mem_unit;
    return 0;
}
@CODE@
int main(void) {
    static const unsigned long long cases[][3] = {
        {8192, 0, 1}, {8192, 2047, 1}, {8192, 2048, 0}, {16384, 0, 0}, {10239, 0, 1}, {10240, 0, 0}};
    char text[512];
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        const int got = memory_notice_text(cases[i][0], cases[i][1], text, sizeof text);
        printf("case %llu MB + %llu MB swap: %s\n", cases[i][0], cases[i][1], got ? "notice" : "none");
        if (got != (int)cases[i][2]) return printf("FAIL: wrong answer\n"), 1;
        if (got && (!strstr(text, "8.0 GB") && cases[i][0] == 8192)) return printf("FAIL: the text has no size: %s\n", text), 1;
    }
    setenv("HOS_HEADLESS", "1", 1);
    memory_notice();
    printf("boxes %d\n", boxes);
    return 0;
}
