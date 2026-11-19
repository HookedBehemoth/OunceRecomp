// (Windows) The executable's functions the renderer calls (api/host_exports.h): each a thunk to the executable's own
// export, found by name at its first call. A DLL cannot leave them to the loader as ounce_renderer.so does on Linux,
// whose undefined os_* bind to the executable's at dlopen (runtime/os/os_exports_windows.c is the other side).
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>

#include "os.h"
#include "os_guest.h"
#include "host_exports.h"

namespace {
void* host_sym(const char* name) {
    void* p = (void*)GetProcAddress(GetModuleHandleW(nullptr), name);
    if (!p) {
        std::fprintf(stderr, "ounce_renderer: the executable does not export %s (runtime/os/os_exports_windows.c)\n", name);
        std::abort();
    }
    return p;
}
}  // namespace

#define THUNK_ROW(ret, name, params, args)                                                  \
    extern "C" ret name params {                                                            \
        static ret(*const host_fn_) params = reinterpret_cast<ret(*) params>(host_sym(#name));   \
        return host_fn_ args;                                                                    \
    }
MKWVK_HOST_EXPORTS(THUNK_ROW)
#endif
