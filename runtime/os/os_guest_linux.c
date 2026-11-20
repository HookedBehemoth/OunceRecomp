/* The guest window on Linux (os_guest.h): a memfd mapped MAP_SHARED at the window's address, so that its blocks count
   the window's memory (os_guest_resident) and MADV_REMOVE gives pages back. MAP_NORESERVE: pages are made on first
   touch. MADV_NOHUGEPAGE: a touch makes one 4 KB page, not a 2 MB one. */
#if defined(__linux__)   /* (sources.txt lists both systems' files: each compiles on its own) */
#include "os_guest.h"
#include "os.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static int memfd = -1;

int os_guest_reserve(void *base, uint64_t size) {
    const int fd = (int)syscall(SYS_memfd_create, "ounce-guest", MFD_CLOEXEC);
    if (fd < 0) return OS_EFAIL;
    if (ftruncate(fd, (off_t)size) == 0) {
        void *p = mmap(base, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE | MAP_NORESERVE, fd, 0);
        if (p == base) {
            memfd = fd;
            madvise(base, size, MADV_NOHUGEPAGE);
            /* (out of a core dump: writing one reads every page of the window, and a read of a page never touched makes
               it, so an abort grew the memfd by gigabytes a second, to the whole memory of the PC) */
            madvise(base, size, MADV_DONTDUMP);
            return 0;
        }
        if (p != MAP_FAILED) munmap(p, size);
    }
    close(fd);
    return OS_EFAIL;
}

int os_guest_commit(void *p, size_t n) { (void)p, (void)n; return 0; }
void os_guest_discard(void *p, size_t n) { madvise(p, n, MADV_REMOVE); }

uint64_t os_guest_resident(void) {
    struct stat st;
    return memfd >= 0 && !fstat(memfd, &st) ? (uint64_t)st.st_blocks * 512 : 0;
}
#endif
