/* The guest window: the game's address space reserved at a fixed host address, its pages made on first touch, and
   ranges of it handed to the GPU without a copy, as a handle whose kind the platform picks and the renderer imports.
   Linux (os_guest_linux.c): a memfd's shared mapping, its ranges exported as dma-bufs through udmabuf. Host pointers
   (VK_EXT_external_memory_host) are not used there: amdgpu walks every page of a host-pointer import again at each
   submission (~3.4 ms per GB imported on RADV, against ~0.05 ms for dma-bufs).

   Windows (not written): host pointers of the window's pages, which WDDM drivers are expected to pin once at the
   import. What differs from Linux: pages are not made on first touch (MAP_NORESERVE): the window is reserved
   (MEM_RESERVE) and ranges committed before the game or an import uses them; a committed range under an import is
   never decommitted (the runtime already zeroes imported ranges instead of discarding them); the window must get the
   fixed base address, which has to be free at start. Not known yet: whether NVIDIA's and AMD's Windows drivers import
   pages of a section view (MapViewOfFile3) rather than VirtualAlloc memory, and whether they bind imported host memory
   into sparse buffers (RADV does). */
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
