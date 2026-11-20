/* The guest window: the game's address space reserved at a fixed host address, its pages made on first touch.
   Linux (os_guest_linux.c): a memfd's shared mapping, whose blocks count the window's memory.
   Windows (os_guest_windows.c): reserved at its address (MEM_RESERVE) and committed as the runtime hands ranges out
   (os_guest_commit, from hos_commit) or, for anything else the game touches, on the first touch (a fault handler commits
   the 64 KB around it). A discard decommits and commits again: the pages read as zero and hold no memory until they are
   touched. */
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
/* pages of the window made usable before the game (or a system call on its behalf) touches them: 0 or < 0 (Linux:
   nothing to do, a page is made at its first touch) */
int os_guest_commit(void *p, size_t n);
/* pages of the window given back: their contents are dropped and they read as zero again */
void os_guest_discard(void *p, size_t n);
uint64_t os_guest_resident(void);   /* the bytes of the window that hold memory now; 0 when not known */

#ifdef __cplusplus
}
#endif
#endif
