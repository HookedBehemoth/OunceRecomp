/* C++ exceptions raised by the game stop it, loudly. Unwinding through lifted code cannot work: the guest unwinder's
   resume returns into the host frames instead of a landing pad (docs/lifter-abi-census.md section 3), so a throw
   that went on would continue somewhere wrong. __cxa_throw, __cxa_rethrow, _Unwind_RaiseException and
   _Unwind_ForcedUnwind are a fatal error with the thrown type (__cxa_throw's type_info name, read from guest memory)
   and the caller's guest address; hos_fatal names the thread. No run has thrown (the kokiri and market runs count 0);
   the main module's one catch (f_823b01c0, a catch (...) that destroys a vector's elements and rethrows) is reached
   only from libc++'s length errors. */
#include "hos.h"
#include <stdint.h>

/* (a NUL-terminated guest string, at most size - 1 bytes; "?" when the address is outside guest memory) */
static const char *guest_str(uint64_t a, char *out, size_t size) {
    size_t n = 0;
    for (const char *p; a && n + 1 < size && (p = hos_guest_range(a + n, 1)) && *p; n++) out[n] = *p;
    if (!n) return "?";
    out[n] = 0;
    return out;
}

ENTRY_HOOK_SYM(__cxa_throw) {   /* (x0 the exception object, x1 its std::type_info, x2 its destructor) */
    char name[160];
    const uint64_t ti = a->x[1];
    const uint64_t *name_at = ti ? hos_guest_range(ti + 8, 8) : 0;   /* (type_info: vtable, then the name) */
    const uint64_t at = name_at ? *name_at : 0;
    hos_fatal(NULL, "the game threw a C++ exception of type %s from %#llx (unwinding through lifted code cannot work)",
              guest_str(at, name, sizeof name), (unsigned long long)a->lr);
    return 1;
}
ENTRY_HOOK_SYM(__cxa_rethrow) {
    hos_fatal(NULL, "the game rethrew a C++ exception from %#llx (unwinding through lifted code cannot work)",
              (unsigned long long)a->lr);
    return 1;
}
ENTRY_HOOK_SYM(_Unwind_RaiseException) {
    hos_fatal(NULL, "_Unwind_RaiseException from %#llx (unwinding through lifted code cannot work)",
              (unsigned long long)a->lr);
    return 1;
}
ENTRY_HOOK_SYM(_Unwind_ForcedUnwind) {
    hos_fatal(NULL, "_Unwind_ForcedUnwind from %#llx (unwinding through lifted code cannot work)",
              (unsigned long long)a->lr);
    return 1;
}
