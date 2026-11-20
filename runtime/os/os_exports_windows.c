/* (Windows) the executable's exports for ounce_renderer.dll: exactly the functions renderer/api/host_exports.h lists,
   which the renderer finds by name (renderer/vk_host_windows.cpp). Linux exports them by pattern at the link instead
   (ounce/builder link.rs). */
#if defined(_WIN32)   /* (sources.txt lists both systems' files: each compiles on its own) */
#include "os.h"
#include "host_exports.h"

#define EXPORT_ROW(ret, name, params, args) __pragma(comment(linker, "/EXPORT:" #name))
MKWVK_HOST_EXPORTS(EXPORT_ROW)
#endif
