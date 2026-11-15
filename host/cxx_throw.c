/* C++ exceptions raised by the game: each call of __cxa_throw, __cxa_rethrow, _Unwind_RaiseException,
   _Unwind_ForcedUnwind and _Unwind_Resume logged ("throw: <name> #<n> from <lr>": the first 16 of each, then every
   power of two, so the last line of a name holds its count within a factor of two), then the game's own code runs
   as before. hos_cxx_throw_totals() logs the exact counts in one line ("throws: ..."; a bench run's end calls it).
   Unwinding through lifted code has never worked (docs/lifter-abi-census.md section 3): these lines say whether a
   run tried. */
#include "hos.h"
#include <stdint.h>

static const char *const NAMES[] = {"__cxa_throw", "__cxa_rethrow", "_Unwind_RaiseException", "_Unwind_ForcedUnwind",
                                    "_Unwind_Resume"};
static volatile long counts[5];

static int seen(Ctx *c, int k) {
    const long n = __atomic_add_fetch(&counts[k], 1, __ATOMIC_RELAXED);
    if (n <= 16 || !(n & (n - 1)))
        hos_log("throw: %s #%ld from %#llx", NAMES[k], n, (unsigned long long)c->x[30]);
    return 0;
}

void hos_cxx_throw_totals(void) {
    hos_log("throws: %s %ld, %s %ld, %s %ld, %s %ld, %s %ld", NAMES[0], counts[0], NAMES[1], counts[1], NAMES[2],
            counts[2], NAMES[3], counts[3], NAMES[4], counts[4]);
}

ENTRY_HOOK_SYM(__cxa_throw) { return seen(c, 0); }
ENTRY_HOOK_SYM(__cxa_rethrow) { return seen(c, 1); }
ENTRY_HOOK_SYM(_Unwind_RaiseException) { return seen(c, 2); }
ENTRY_HOOK_SYM(_Unwind_ForcedUnwind) { return seen(c, 3); }
ENTRY_HOOK_SYM(_Unwind_Resume) { return seen(c, 4); }
