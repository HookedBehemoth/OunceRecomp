/* The renderer's window calls, made by the host's window code (host/display.c, host/posix/display_sdl.c) and answered by
   host/rx_backend.c. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The window to present into (an MkwVkWindow on Linux), from the window's thread once it exists; null: no window (the
   renderer draws offscreen). Returns 1 when the renderer runs. */
int rx_start(void *hwnd);
/* The window's client area changed size. */
void rx_resize(int width, int height);

#ifdef __cplusplus
}
#endif
