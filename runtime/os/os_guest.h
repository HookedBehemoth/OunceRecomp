/* The guest window: the game's address space reserved at a fixed host address, its pages made on first touch, and
   ranges of it handed to the GPU without a copy, as a handle whose kind the platform picks and the renderer imports.
   Linux (os_guest_linux.c): a memfd's shared mapping, its ranges exported as dma-bufs through udmabuf. Host pointers
   (VK_EXT_external_memory_host) are not used there: amdgpu walks every page of a host-pointer import again at each
   submission (~3.4 ms per GB imported on RADV, against ~0.05 ms for dma-bufs).

   Windows (os_guest_windows.c): the window reserved at its address (MEM_RESERVE) and committed as the runtime hands
   ranges out (os_guest_commit, from hos_commit) or, for anything else the game touches, on the first touch (a fault
   handler commits the 64 KB around it); ranges are exported as host pointers of the window's own pages
   (VK_EXT_external_memory_host), committed first, and an exported range is never decommitted (a discard there zeroes
   it, as the runtime does for ranges the renderer imported). A discard elsewhere decommits and commits again: the pages
   read as zero and hold no memory until they are touched. */
#ifndef OUNCE_OS_GUEST_H
#define OUNCE_OS_GUEST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* the window at exactly base, readable and writable: 0 or < 0 */
int os_guest_reserve(void *base, uint64_t size);
/* pages of the window made usable before the game (or a system call on its behalf, or an import) touches them:
   0 or < 0 (Linux: nothing to do, a page is made at its first touch) */
int os_guest_commit(void *p, size_t n);
/* pages of the window given back: their contents are dropped and they read as zero again */
void os_guest_discard(void *p, size_t n);
uint64_t os_guest_resident(void);   /* the bytes of the window that hold memory now; 0 when not known */

/* ---- the GPU's view: a range of the window as a handle the renderer imports by its kind ---- */
enum { OS_GPU_NONE = 0, OS_GPU_DMABUF_FD = 1, OS_GPU_HOST_POINTER = 2 };
typedef struct { int kind; int fd; void *ptr; uint64_t size; } os_gpu_handle;   /* (fd: DMABUF_FD; ptr: HOST_POINTER) */
/* the window can be exported: the largest range one export takes; *note (may be NULL) says why not */
bool os_guest_exportable(uint64_t *max_bytes, const char **note);
/* [offset, offset + size) of the window: 0 or < 0. The handle is the caller's until an import takes it */
int os_guest_export(uint64_t offset, uint64_t size, os_gpu_handle *out);
void os_gpu_handle_close(os_gpu_handle *h);

#ifdef __cplusplus
}
#endif
#endif
