/* The guest window on Linux (os_guest.h). Shared: a sealed memfd mapped MAP_SHARED at the window's address, its ranges
   exported as dma-bufs with udmabuf (UDMABUF_CREATE), discarded with MADV_REMOVE. Private: anonymous memory, discarded
   with MADV_DONTNEED. Either way MAP_NORESERVE (pages made on first touch) and MADV_NOHUGEPAGE: a huge-page collapse
   under a GPU import makes the kernel pin the pages again at the next submission (~250 ms a submit on RADV). The
   memfd is chosen only where udmabuf works: a shared mapping's pages cannot be host-pointer imports. */
#include "os_guest.h"
#include "os.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static int memfd = -1, udmabuf = -1;
static uint64_t udmabuf_limit;   /* (bytes; size_limit_mb) */

/* /dev/udmabuf open and its size limit read: 1, else 0 with *note set */
static int udmabuf_open(const char **note) {
    if (udmabuf >= 0) return 1;
    const int u = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (u < 0) { if (note) *note = "/dev/udmabuf cannot be opened"; return 0; }
    unsigned long long mb = 0;
    FILE *f = fopen("/sys/module/udmabuf/parameters/size_limit_mb", "r");
    if (f) {
        if (fscanf(f, "%llu", &mb) != 1) mb = 0;
        fclose(f);
    }
    if (!mb) { close(u); if (note) *note = "udmabuf's size_limit_mb cannot be read"; return 0; }
    udmabuf = u, udmabuf_limit = (uint64_t)mb << 20;
    return 1;
}

int os_guest_reserve(void *base, uint64_t size, int share, const char **note) {
    if (note) *note = NULL;
    const int can = udmabuf_open(note);
    if (share == OS_GUEST_SHARE_ON || (share == OS_GUEST_SHARE_AUTO && can)) {
        const int fd = (int)syscall(SYS_memfd_create, "ounce-guest", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (fd >= 0 && ftruncate(fd, (off_t)size) == 0) {
            fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW);
            void *p = mmap(base, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE | MAP_NORESERVE, fd, 0);
            if (p == base) {
                memfd = fd;
                madvise(base, size, MADV_NOHUGEPAGE);
                return OS_GUEST_SHARED;
            }
            if (p != MAP_FAILED) munmap(p, size);
        }
        if (fd >= 0) close(fd);
        if (note) *note = "no memfd for the window";
    } else if (share == OS_GUEST_SHARE_OFF && note) {
        *note = "not asked for";
    }
    if (mmap(base, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0) != base)
        return OS_EFAIL;
    madvise(base, size, MADV_NOHUGEPAGE);
    return OS_GUEST_PRIVATE;
}

void os_guest_discard(void *p, size_t n) { madvise(p, n, memfd >= 0 ? MADV_REMOVE : MADV_DONTNEED); }

uint64_t os_guest_resident(void) {
    struct stat st;
    return memfd >= 0 && !fstat(memfd, &st) ? (uint64_t)st.st_blocks * 512 : 0;
}

bool os_guest_exportable(uint64_t *max_bytes) {
    if (memfd < 0 || udmabuf < 0) return false;
    if (max_bytes) *max_bytes = udmabuf_limit;
    return true;
}

struct udmabuf_create { uint32_t memfd, flags; uint64_t offset, size; };
#define UDMABUF_CREATE _IOW('u', 0x42, struct udmabuf_create)
#define UDMABUF_FLAGS_CLOEXEC 0x01

int os_guest_export(uint64_t offset, uint64_t size, os_gpu_handle *out) {
    out->kind = OS_GPU_NONE, out->fd = -1, out->ptr = NULL, out->size = 0;
    if (memfd < 0 || udmabuf < 0) return OS_EINVAL;
    struct udmabuf_create c = {(uint32_t)memfd, UDMABUF_FLAGS_CLOEXEC, offset, size};
    const int fd = ioctl(udmabuf, UDMABUF_CREATE, &c);
    if (fd < 0) return errno == ENOMEM ? OS_ENOMEM : OS_EFAIL;
    out->kind = OS_GPU_DMABUF_FD, out->fd = fd, out->size = size;
    return 0;
}

void os_gpu_handle_close(os_gpu_handle *h) {
    if (h->kind == OS_GPU_DMABUF_FD && h->fd >= 0) close(h->fd);
    h->kind = OS_GPU_NONE, h->fd = -1, h->ptr = NULL;
}
