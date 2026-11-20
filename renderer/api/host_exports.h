/* What ounce_renderer calls in the game executable: os.h's functions and hos_mem_ledger, one row
   each as X(return type, name, (parameters), (arguments)).

   On Linux the executable exports them by pattern (--export-dynamic-symbol=os_*) and the loader binds the library's
   undefined symbols to them. On Windows a DLL cannot import from an executable whose name it does not know, so this
   list is both sides of the link there: runtime/os/os_exports_windows.c exports exactly these from the executable,
   and renderer/vk_host_windows.cpp defines each as a thunk to the executable's export, found by name at its first call
   (a missing one stops the renderer, naming it). A call the renderer makes outside this list is an unresolved symbol
   at the renderer's link on Windows: add its row here. Include os.h first. */
#ifndef MKWVK_HOST_EXPORTS_H
#define MKWVK_HOST_EXPORTS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
size_t hos_mem_ledger(char *out, size_t cap, uint64_t *ours, uint64_t *guest_committed);   /* (runtime/hos_mem.c) */
#ifdef __cplusplus
}
#endif

#define MKWVK_HOST_EXPORTS(X)                                                                                         \
    X(int, os_cpu_count, (void), ())                                                                                  \
    X(int, os_exe_path, (char *out, size_t cap), (out, cap))                                                          \
    X(void, os_exit, (int code), (code))                                                                              \
    X(void, os_file_close, (os_file *f), (f))                                                                         \
    X(const void *, os_file_map_read, (os_file *f, size_t *size), (f, size))                                          \
    X(os_file *, os_file_open, (const char *path, int flags, int *err), (path, flags, err))                           \
    X(void, os_file_random, (os_file *f), (f))                                                                        \
    X(int64_t, os_file_read_at, (os_file *f, void *buf, size_t n, uint64_t at), (f, buf, n, at))                      \
    X(int, os_file_resize, (os_file *f, uint64_t size), (f, size))                                                    \
    X(int64_t, os_file_size, (os_file *f), (f))                                                                       \
    X(bool, os_file_trylock, (os_file *f), (f))                                                                       \
    X(int64_t, os_file_write_at, (os_file *f, const void *buf, size_t n, uint64_t at), (f, buf, n, at))               \
    X(os_lib *, os_lib_open, (const char *path), (path))                                                              \
    X(void *, os_lib_sym, (os_lib *l, const char *name), (l, name))                                                   \
    X(void, os_mem_use_get, (os_mem_use *out), (out))                                                                 \
    X(int, os_mkdir, (const char *path), (path))                                                                      \
    X(int64_t, os_now_ns, (void), ())                                                                                 \
    X(int, os_rename, (const char *from, const char *to, bool replace), (from, to, replace))                         \
    X(os_thread *, os_thread_current, (void), ())                                                                     \
    X(void, os_thread_name, (os_thread *t, char *out, size_t cap), (t, out, cap))                                     \
    X(int, os_thread_priority, (os_thread *t), (t))                                                                   \
    X(void, os_thread_release, (os_thread *t), (t))                                                                   \
    X(bool, os_thread_set_priority, (os_thread *t, int prio), (t, prio))                                              \
    X(os_thread *, os_thread_start, (os_thread_fn fn, void *arg, size_t stack, const char *name), (fn, arg, stack, name)) \
    X(uint32_t, os_tid, (void), ())                                                                                   \
    X(size_t, hos_mem_ledger, (char *out, size_t cap, uint64_t *ours, uint64_t *guest_committed), (out, cap, ours, guest_committed))
#endif
