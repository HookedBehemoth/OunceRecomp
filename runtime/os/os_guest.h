/* The guest window: the game's address space reserved at a fixed host address, its pages made on first touch, and
   ranges of it handed to the GPU without a copy. Implemented by os_guest_linux.c (a memfd's shared mapping, exported as
   dma-bufs through udmabuf; anonymous memory when the GPU cannot take those). */
#ifndef OUNCE_OS_GUEST_H
#define OUNCE_OS_GUEST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { OS_GUEST_SHARE_AUTO = 0, OS_GUEST_SHARE_ON = 1, OS_GUEST_SHARE_OFF = 2 };   /* (AUTO: when the export works) */
enum { OS_GUEST_PRIVATE = 0, OS_GUEST_SHARED = 1 };
/* the window at exactly base, readable and writable: OS_GUEST_SHARED (exportable), OS_GUEST_PRIVATE or < 0. *note (may
   be NULL) says why it is not shared, NULL when it is or nothing is to say */
int os_guest_reserve(void *base, uint64_t size, int share, const char **note);
/* pages of the window given back: their contents are dropped and they read as zero again */
void os_guest_discard(void *p, size_t n);
uint64_t os_guest_resident(void);   /* the bytes of the window that hold memory now; 0 when not known */

/* ---- the GPU's view: a range of the window as a handle the renderer imports by its kind ---- */
enum { OS_GPU_NONE = 0, OS_GPU_DMABUF_FD = 1, OS_GPU_HOST_POINTER = 2 };
typedef struct { int kind; int fd; void *ptr; uint64_t size; } os_gpu_handle;   /* (fd: DMABUF_FD; ptr: HOST_POINTER) */
bool os_guest_exportable(uint64_t *max_bytes);   /* the window can be exported; the largest range one export takes */
/* [offset, offset + size) of the window: 0 or < 0. The handle is the caller's until an import takes it */
int os_guest_export(uint64_t offset, uint64_t size, os_gpu_handle *out);
void os_gpu_handle_close(os_gpu_handle *h);

#ifdef __cplusplus
}
#endif
#endif
