/* (Windows) the POSIX names the host's C (runtime/, the games' patches, the a64-kit runtime) takes from libc, as the
   UCRT spells them. The builder force-includes this header into every host compile on Windows (builder host.rs), and
   puts win/ on the include path for the POSIX headers the UCRT lacks (<strings.h>), so the sources keep one spelling;
   nothing here is used on Linux. */
#ifndef OUNCE_OS_CRT_WINDOWS_H
#define OUNCE_OS_CRT_WINDOWS_H
#ifdef _WIN32
#define _USE_MATH_DEFINES   /* (M_PI and its kin from <math.h>) */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define timegm _mkgmtime
#define realpath(path, out) _fullpath((out), (path), 4096)

static inline int setenv(const char *name, const char *value, int replace) {
    if (!replace && getenv(name)) return 0;
    return _putenv_s(name, value) ? -1 : 0;
}
static inline struct tm *localtime_r(const time_t *t, struct tm *out) { return localtime_s(out, t) ? NULL : out; }
static inline struct tm *gmtime_r(const time_t *t, struct tm *out) { return gmtime_s(out, t) ? NULL : out; }

#define ftello _ftelli64
#define fseeko _fseeki64
#define malloc_usable_size _msize

/* clock_gettime's monotonic clock: the performance counter (os_windows.c) */
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
int os_crt_clock_gettime(int clock, struct timespec *ts);
#define clock_gettime os_crt_clock_gettime
#endif
#endif
#endif
