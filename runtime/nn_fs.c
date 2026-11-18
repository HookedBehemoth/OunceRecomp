/* nn::fs: mounts map to host folders.
     rom:      the game's RomFS (extracted by tools/nx.py romfs), read-only
     save:     <user>/save/<uid>     (MountSaveData / MountSaveDataReadOnly)
     cache:    <user>/cache/<index>  (MountCacheStorage)
     temp:     <user>/temp           (MountTemporaryStorage)
   hos_romfs_dir and hos_user_dir are set by hos_main.c. Reads are positional (os_file_read_at), so any number
   of guest threads can read one file at once. Results: switchbrew "Error codes" (module 2 = fs). */
#include "hos.h"
#include "crit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"
#include <strings.h>

/* (runtime, #1468) A game folder whose deepest file passes 260 characters (MAX_PATH: "E:\Games\...\romfs\Event\EventSetting\
   Ev..." at 262) is read too. The host paths here are up to HOS_FS_PATH bytes, UTF-8 as the
   game's strings are. */
#define HOS_FS_PATH 1024

const char *hos_romfs_dir, *hos_user_dir;

#define R_PATH_NOT_FOUND 0x202          /* fs::ResultPathNotFound */
#define R_PATH_EXISTS 0x402             /* fs::ResultPathAlreadyExists */
#define R_NOT_MOUNTED 0xCA02            /* fs::ResultInvalidMountName-ish: no such mount */
#define R_TARGET_NOT_FOUND 0x7D402      /* fs::ResultTargetNotFound (save data) */

#define FS_PATH_MAX 0x302                /* (nn::fs paths: 0x300 bytes and the NUL; DirectoryEntry's name is 0x301) */

typedef struct { char name[32]; char root[HOS_FS_PATH]; int writable; } Mount;
static Mount mounts[16];
static int nmounts;
static os_rwlock mount_lock = OS_RWLOCK_INIT;

static int mount(const char *name, const char *root, int writable) {   /* 0: the table is full (16 mounts) */
    os_rwlock_lock(&mount_lock);
    int i;
    for (i = 0; i < nmounts && strcmp(mounts[i].name, name); i++) {}
    if (i == nmounts) {   /* (runtime: a 17th name was written past the table) */
        if (nmounts == 16) {
            os_rwlock_unlock(&mount_lock);
            hos_log("fs: %s: not mounted (16 mounts already)", name);
            return 0;
        }
        nmounts++;
    }
    snprintf(mounts[i].name, sizeof mounts[i].name, "%s", name);
    snprintf(mounts[i].root, sizeof mounts[i].root, "%s", root);
    mounts[i].writable = writable;
    os_rwlock_unlock(&mount_lock);
    if (writable) os_mkdir(root);
    hos_log("fs: mounted %s: -> %s", name, root);
    return 1;
}

int hos_mods_romfs(const char *rel, char *out, size_t cap);   /* mods.c */
int hos_mods_dir_extras(const char *rel, const char *base_dir, void (*fn)(void *, const char *, int, uint64_t), void *arg);

/* (runtime) a guest path below its mount stays below it on the host: no "." or ".." part, no control characters */
static int guest_path_ok(const char *rest) {
    for (const char *p = rest; *p;) {
        const char *e = strchr(p, '/');
        const size_t n = e ? (size_t)(e - p) : strlen(p);
        if ((n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return 0;
        for (size_t i = 0; i < n; i++)
            if ((unsigned char)p[i] < 32) return 0;
        p += n;
        while (*p == '/') p++;
    }
    return 1;
}

/* "mount:/a/b" -> host path (a mod's file instead of the RomFS one, when a mod has it: mods.c). Returns 0 if the mount
   is unknown, or the path leaves it (guest_path_ok). */
static int host_path(const char *guest_mem, char *out, size_t cap, int *writable) {
    /* (runtime) one copy of the path, read once: another guest thread may change guest memory between the check and
       the use */
    char guest[FS_PATH_MAX];
    if ((size_t)snprintf(guest, sizeof guest, "%s", guest_mem) >= sizeof guest) return 0;
    const char *colon = strchr(guest, ':');
    if (!colon) return 0;
    size_t n = (size_t)(colon - guest);
    int ok = 0;
    os_rwlock_lock_shared(&mount_lock);
    for (int i = 0; i < nmounts; i++) {
        if (strlen(mounts[i].name) == n && !strncmp(mounts[i].name, guest, n)) {
            const char *rest = colon + 1;
            while (*rest == '/') rest++;
            if (!guest_path_ok(rest)) break;
            if (mounts[i].writable || !hos_mods_romfs(rest, out, cap))
                if ((size_t)snprintf(out, cap, "%s/%s", mounts[i].root, rest) >= cap) break;   /* (cut short: not this path) */
            if (writable) *writable = mounts[i].writable;
            ok = 1;
            break;
        }
    }
    os_rwlock_unlock_shared(&mount_lock);
    return ok;
}


/* (runtime) a guest string: "" unless it lies in guest memory and ends within FS_PATH_MAX bytes */
static const char *gstr(uint64_t a) {
    if (!a || a >= (uint64_t)HOS_SPACE_END) return "";
    const uint64_t room = (uint64_t)HOS_SPACE_END - a;
    const char *s = (const char *)GP(a);
    const size_t max = room < FS_PATH_MAX ? (size_t)room : FS_PATH_MAX;
    return strnlen(s, max) < max ? s : "";
}

static void make_dirs(const char *path) {   /* every parent of path */
    char tmp[HOS_FS_PATH];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; os_mkdir(tmp); *p = '/'; }
}

/* ---- handles: a FileHandle / DirectoryHandle is one pointer-sized value; ours are table indices + 1 ----------- */
typedef struct { char name[HOS_FS_PATH]; int is_dir; uint64_t size; } ExtraEntry;   /* (a listed folder's mod-only entries) */
typedef struct {
    os_file *f; int writable; os_dir *dir; os_dirent de; int find_done, is_dir; char path[HOS_FS_PATH];
    ExtraEntry *extras; int nextras, extra_pos;
} Handle;
static Handle *handles[4096];
static os_mutex handle_lock = OS_MUTEX_INIT;

static uint64_t new_handle(Handle *h) {
    os_mutex_lock(&handle_lock);
    for (int i = 0; i < 4096; i++)
        if (!handles[i]) { handles[i] = h; os_mutex_unlock(&handle_lock); return (uint64_t)i + 1; }
    os_mutex_unlock(&handle_lock);
    hos_fatal(NULL, "fs: out of handles");
    return 0;
}
static Handle *get_handle(Ctx *c, uint64_t id) {
    Handle *h = id && id <= 4096 ? handles[id - 1] : NULL;
    if (!h) hos_fatal(c, "fs: bad handle %#llx", (unsigned long long)id);
    return h;
}
static void free_handle(uint64_t id) {
    os_mutex_lock(&handle_lock);
    if (id && id <= 4096) handles[id - 1] = NULL;
    os_mutex_unlock(&handle_lock);
}

/* ---- mounts --------------------------------------------------------------------------------------------------- */
HLE(_ZN2nn2fs22QueryMountRomCacheSizeEPm) { G64(ARG(0)) = 0x10000; RET(0); }
HLE(_ZN2nn2fs8MountRomEPKcPvm) {
    if (!hos_romfs_dir) hos_fatal(c, "fs: MountRom but no RomFS folder given (--romfs)");
    RET(mount(gstr(ARG(0)), hos_romfs_dir, 0) ? 0 : R_NOT_MOUNTED);
}
/* (runtime) MountRom(char const* name): the cacheless overload OoT uses (SDK 22). Left to the SDK it opens the
   file-system proxy over IPC and aborts on the empty reply. */
HLE(_ZN2nn2fs8MountRomEPKc) {
    if (!hos_romfs_dir) hos_fatal(c, "fs: MountRom but no RomFS folder given (--romfs)");
    RET(mount(gstr(ARG(0)), hos_romfs_dir, 0) ? 0 : R_NOT_MOUNTED);
}

static int user_mount(const char *name, const char *sub) {
    char root[HOS_FS_PATH];
    snprintf(root, sizeof root, "%s/%s", hos_user_dir, sub);
    make_dirs(root);
    os_mkdir(root);
    return mount(name, root, 1);
}

/* nn::fs::IsSaveDataExisting(ApplicationId, account::Uid const&) -> bool */
HLE(_ZN2nn2fs18IsSaveDataExistingENS_13ApplicationIdERKNS_7account3UidE) {
    char root[HOS_FS_PATH];
    snprintf(root, sizeof root, "%s/save/%016llx%016llx", hos_user_dir, (unsigned long long)G64(ARG(1) + 8),
             (unsigned long long)G64(ARG(1)));
    RET(os_path_kind(root) != OS_PATH_NONE);
}
/* MountSaveData(char const*, account::Uid const&) */
HLE(_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE) {
    char sub[64];
    snprintf(sub, sizeof sub, "save/%016llx%016llx", (unsigned long long)G64(ARG(1) + 8), (unsigned long long)G64(ARG(1)));
    RET(user_mount(gstr(ARG(0)), sub) ? 0 : R_NOT_MOUNTED);
}
/* MountSaveDataReadOnly(char const*, ApplicationId, account::Uid const&) */
HLE(_ZN2nn2fs21MountSaveDataReadOnlyEPKcNS_13ApplicationIdERKNS_7account3UidE) {
    char sub[64];
    snprintf(sub, sizeof sub, "save/%016llx%016llx", (unsigned long long)G64(ARG(2) + 8), (unsigned long long)G64(ARG(2)));
    RET(user_mount(gstr(ARG(0)), sub) ? 0 : R_NOT_MOUNTED);
}
HLE(_ZN2nn2fs21MountTemporaryStorageEPKc) { RET(user_mount(gstr(ARG(0)), "temp") ? 0 : R_NOT_MOUNTED); }
/* MountCacheStorage(char const*, int index) */
HLE(_ZN2nn2fs17MountCacheStorageEPKci) {
    char sub[32];
    snprintf(sub, sizeof sub, "cache/%d", (int)ARG(1));
    RET(user_mount(gstr(ARG(0)), sub) ? 0 : R_NOT_MOUNTED);
}
/* CreateCacheStorage(int index, long size, long journal) */
HLE(_ZN2nn2fs18CreateCacheStorageEill) {
    char root[HOS_FS_PATH];
    snprintf(root, sizeof root, "%s/cache/%d/", hos_user_dir, (int)ARG(0));
    make_dirs(root);
    RET(0);
}
HLE(_ZN2nn2fs18DeleteCacheStorageEi) { RET(0); }
/* The cache-storage list: report the indices that exist on disk. Handle: one value; info: {s32 index, pad[0x1C]} */
static int cache_list_pos;
HLE(_ZN2nn2fs20OpenCacheStorageListEPNS0_22CacheStorageListHandleE) { G64(ARG(0)) = 1; cache_list_pos = 0; RET(0); }
HLE(_ZN2nn2fs20ReadCacheStorageListEPiPNS0_16CacheStorageInfoENS0_22CacheStorageListHandleEi) {
    int n = 0, max = (int)ARG(3);
    for (; cache_list_pos < 16 && n < max; cache_list_pos++) {
        char root[HOS_FS_PATH];
        snprintf(root, sizeof root, "%s/cache/%d", hos_user_dir, cache_list_pos);
        if (os_path_kind(root) != OS_PATH_NONE) {
            memset(GP(ARG(1) + 0x20 * n), 0, 0x20);
            G32(ARG(1) + 0x20 * n) = (uint32_t)cache_list_pos;
            n++;
        }
    }
    G32(ARG(0)) = (uint32_t)n;
    RET(0);
}
HLE(_ZN2nn2fs21CloseCacheStorageListENS0_22CacheStorageListHandleE) {}
HLE(_ZN2nn2fs7UnmountEPKc) {
    os_rwlock_lock(&mount_lock);
    for (int i = 0; i < nmounts; i++)
        if (!strcmp(mounts[i].name, gstr(ARG(0)))) { mounts[i] = mounts[--nmounts]; break; }
    os_rwlock_unlock(&mount_lock);
}
HLE(_ZN2nn2fs6CommitEPKc) { RET(0); }
HLE(_ZN2nn2fs26SetPriorityOnCurrentThreadENS0_8PriorityE) {}

/* ---- files ---------------------------------------------------------------------------------------------------- */
/* HOS_FS_TRACE=1: every file opened (or not found) is logged */
static int fs_trace(void) {
    static int on = -1;
    if (on < 0) on = getenv("HOS_FS_TRACE") != NULL;
    return on;
}
/* OpenFile(FileHandle*, char const* path, int mode): mode bit 0 read, bit 1 write, bit 2 append */
HLE(_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci) {
    char path[HOS_FS_PATH];
    int writable = 0;
    const char *g = gstr(ARG(1));
    if (!host_path(g, path, sizeof path, &writable)) { hos_log("fs: OpenFile(%s): unknown mount", g); RET(R_PATH_NOT_FOUND); return; }
    int want_write = (ARG(2) & 6) != 0;
    if (want_write && !writable) { RET(R_PATH_NOT_FOUND); return; }
    /* (mkw-recomp) HOS_NO_MOVIES=1: MKW's WebM movies (cup openings, billboards, emblems) are not found, and the
       game goes on without them (decoded by nn_video.c otherwise) */
    static int no_movies = -1;   /* (switches read once: OpenFile runs ~6,000 times in a load) */
    if (no_movies < 0) no_movies = getenv("HOS_NO_MOVIES") != NULL;
    if (!want_write && no_movies) {
        const size_t n = strlen(g);
        if (n > 5 && !strcasecmp(g + n - 5, ".webm")) {
            static int logged;
            if (!logged++) hos_log("fs: OpenFile(%s): movies are left out (HOS_NO_MOVIES)", g);
            RET(R_PATH_NOT_FOUND);
            return;
        }
    }
    {   /* HOS_FS_OVERLAY=<folder> (a test): a file of the game's data found there is read in place of the game's own */
        static const char *ov_env;
        static int ov_read;
        if (!ov_read) ov_env = getenv("HOS_FS_OVERLAY"), ov_read = 1;
        const char *ov = ov_env;
        const char *rel = !want_write && ov ? strstr(g, ":/") : NULL;
        if (rel) {
            char alt[HOS_FS_PATH];
            snprintf(alt, sizeof alt, "%s/%s", ov, rel + 2);
            if (os_path_kind(alt) != OS_PATH_NONE) {
                hos_log("fs: OpenFile(%s): from the overlay", g);
                snprintf(path, sizeof path, "%s", alt);
            }
        }
    }
    /* (the host's file calls block: a block for HOS_DBG=crit) */
    const int away = hos_block_begin(CRIT_BLOCK_OTHER, (const void *)9);
    os_file *f = os_file_open(path, OS_READ | (want_write ? OS_WRITE : 0), NULL);
    if (away) hos_block_end();
    if (!f) {
        if (hos_trace || fs_trace()) hos_log("fs: OpenFile(%s): not found", g);
        RET(R_PATH_NOT_FOUND);
        return;
    }
    Handle *fh = (Handle *)calloc(1, sizeof *fh);
    fh->f = f;
    fh->writable = want_write;
    snprintf(fh->path, sizeof fh->path, "%s", g);
    G64(ARG(0)) = new_handle(fh);
    if (hos_trace || fs_trace()) hos_log("fs: OpenFile(%s)", g);
    RET(0);
}

static uint64_t read_at(Ctx *c, uint64_t id, int64_t off, uint64_t buf, uint64_t size) {
    Handle *fh = get_handle(c, id);
    uint64_t done = 0;
    if (!hos_guest_range(buf, size)) return 0;   /* (runtime: the file's bytes go into guest memory only) */
    if (hos_bench_on()) hos_bench_fs_read();   /* (HOS_BENCH's "no streaming") */
    static int64_t delay_ns = -1, delay_after_ns;   /* HOS_FS_DELAY_MS=<ms>[,<after s>] (experiment): slower reads */
    if (delay_ns < 0) {
        const char *e = getenv("HOS_FS_DELAY_MS");
        delay_ns = e ? (int64_t)(atof(e) * 1e6) : 0;
        const char *a = e ? strchr(e, ',') : NULL;
        delay_after_ns = hos_now_ns() + (a ? (int64_t)(atof(a + 1) * 1e9) : 0);
    }
    if (delay_ns && hos_now_ns() >= delay_after_ns) {
        if (fs_trace()) hos_log("fs: read %s at %lld, %llu bytes", fh->path, (long long)off, (unsigned long long)size);
        hos_sleep_ns(delay_ns);
    }
    /* (as OpenFile: no core held through the read). HOS_FS_KEEP_CORE_KB=<KB> (experiment, native-vk boot speed): reads
       up to that size keep the core. The loading screen's reads come from the page cache in well under a millisecond,
       while taking the core back cost ARes Loading 0 580 ms of 5.5 s (work/assets/native/boot-speed.md) */
    static int64_t keep_core = -1;
    if (keep_core < 0) keep_core = getenv("HOS_FS_KEEP_CORE_KB") ? atoll(getenv("HOS_FS_KEEP_CORE_KB")) * 1024 : 0;
    const int away = (int64_t)size <= keep_core ? 0 : hos_block_begin(CRIT_BLOCK_OTHER, (const void *)10);
    {   /* (M4d: HOS_DBG=texwatch in direct mode: a read into watched texture storage, made writable first) */
        void texwatch_kernel_write(uint64_t guest, uint64_t n, const char *what);
        texwatch_kernel_write(buf, size, fh->path);
    }
    const int64_t got = os_file_read_at(fh->f, GP(buf), (size_t)size, (uint64_t)off);
    if (got > 0) done = (uint64_t)got;
    if (away) hos_block_end();
    {   /* HOS_FS_WATCH=<lo hex>:<hi hex> (debugging): reads into that guest range, with the file */
        static int on = -1;
        static unsigned long long wlo, whi;
        if (on < 0) on = getenv("HOS_FS_WATCH") && sscanf(getenv("HOS_FS_WATCH"), "%llx:%llx", &wlo, &whi) == 2;
        if (on && buf < whi && buf + done > wlo)
            hos_log("fs: read into watched memory: %s at %lld, %llu bytes to %#llx", fh->path, (long long)off,
                    (unsigned long long)done, (unsigned long long)buf);
    }
    return done;
}

/* ReadFile(FileHandle, long offset, void* buf, unsigned long size) -> Result (must read it all) */
HLE(_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm) {
    uint64_t n = read_at(c, ARG(0), (int64_t)ARG(1), ARG(2), ARG(3));
    RET(n == ARG(3) ? 0 : 0x2EE202);   /* fs::ResultOutOfRange */
}
/* ReadFile(unsigned long* read, FileHandle, long offset, void* buf, unsigned long size[, ReadOption const&]) */
HLE(_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm) {
    G64(ARG(0)) = read_at(c, ARG(1), (int64_t)ARG(2), ARG(3), ARG(4));
    RET(0);
}
HLE(_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE) {
    G64(ARG(0)) = read_at(c, ARG(1), (int64_t)ARG(2), ARG(3), ARG(4));
    RET(0);
}
/* GetFileSize(long*, FileHandle) */
HLE(_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE) {
    const int64_t sz = os_file_size(get_handle(c, ARG(1))->f);   /* (runtime: none known: 0, not the host stack's bytes) */
    G64(ARG(0)) = sz > 0 ? (uint64_t)sz : 0;
    RET(0);
}
/* WriteFile(FileHandle, long offset, void const* buf, unsigned long size, WriteOption const&) */
HLE(_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE) {
    Handle *fh = get_handle(c, ARG(0));
    int64_t put = 0;
    /* (runtime) from guest memory only, and the whole size: a 32-bit cut wrote less than asked and said so */
    if (ARG(3) <= 0xFFFFFFFFu && hos_guest_range(ARG(2), ARG(3))) put = os_file_write_at(fh->f, GP(ARG(2)), (size_t)ARG(3), ARG(1));
    RET(put >= 0 && (uint64_t)put == ARG(3) ? 0 : 0x2EE202);
}
HLE(_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl) {
    os_file_resize(get_handle(c, ARG(0))->f, ARG(1));
    RET(0);
}
HLE(_ZN2nn2fs9FlushFileENS0_10FileHandleE) { os_file_flush(get_handle(c, ARG(0))->f); RET(0); }
HLE(_ZN2nn2fs9CloseFileENS0_10FileHandleE) {
    Handle *fh = get_handle(c, ARG(0));
    os_file_close(fh->f);
    free_handle(ARG(0));
    free(fh);
}

/* CreateFile(char const*, long size) */
HLE(_ZN2nn2fs10CreateFileEPKcl) {
    char path[HOS_FS_PATH];
    int writable = 0;
    if (!host_path(gstr(ARG(0)), path, sizeof path, &writable) || !writable) { RET(R_PATH_NOT_FOUND); return; }
    make_dirs(path);
    int err = 0;
    os_file *f = os_file_open(path, OS_WRITE | OS_EXCL, &err);
    if (!f) { RET(err == OS_EEXIST ? R_PATH_EXISTS : R_PATH_NOT_FOUND); return; }
    os_file_resize(f, ARG(1));
    os_file_close(f);
    RET(0);
}
HLE(_ZN2nn2fs10DeleteFileEPKc) {
    char path[HOS_FS_PATH];
    int writable = 0;
    if (!host_path(gstr(ARG(0)), path, sizeof path, &writable) || !writable || os_remove(path)) { RET(R_PATH_NOT_FOUND); return; }
    RET(0);
}
HLE(_ZN2nn2fs15CreateDirectoryEPKc) {
    char path[HOS_FS_PATH];
    int writable = 0;
    if (!host_path(gstr(ARG(0)), path, sizeof path, &writable) || !writable) { RET(R_PATH_NOT_FOUND); return; }
    make_dirs(path);
    const int r = os_mkdir(path);
    RET(!r ? 0 : r == OS_EEXIST ? R_PATH_EXISTS : R_PATH_NOT_FOUND);
}
static void delete_tree(const char *dir) {
    os_dir *d = os_dir_open(dir, NULL);
    os_dirent e;
    while (d && os_dir_next(d, &e)) {
        char sub[HOS_FS_PATH];
        snprintf(sub, sizeof sub, "%s/%s", dir, e.name);
        /* (runtime) a link (a symbolic link, a junction) goes itself: never what it points to */
        if (e.kind & OS_PATH_LINK) { if (os_remove(sub)) os_rmdir(sub); }
        else if (e.kind & OS_PATH_DIR) delete_tree(sub);
        else os_remove(sub);
    }
    os_dir_close(d);
    os_rmdir(dir);
}
HLE(_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc) {
    char path[HOS_FS_PATH];
    int writable = 0;
    if (!host_path(gstr(ARG(0)), path, sizeof path, &writable) || !writable) { RET(R_PATH_NOT_FOUND); return; }
    /* (runtime) the folder named itself a link: the link goes, never what it points to (as for the links inside) */
    if (os_path_kind(path) & OS_PATH_LINK) {
        if (os_remove(path)) os_rmdir(path);
        RET(0);
        return;
    }
    delete_tree(path);
    RET(0);
}
HLE(_ZN2nn2fs10RenameFileEPKcS2_) {
    char a[HOS_FS_PATH], b[HOS_FS_PATH];
    int w1 = 0, w2 = 0;
    if (!host_path(gstr(ARG(0)), a, sizeof a, &w1) || !host_path(gstr(ARG(1)), b, sizeof b, &w2) || !w1 || !w2) { RET(R_PATH_NOT_FOUND); return; }
    RET(os_rename(a, b, true) ? R_PATH_NOT_FOUND : 0);
}
/* a RomFS folder that only mods have: rom:/x -> some mod entry lies under x */
static void count_extra(void *arg, const char *name, int is_dir, uint64_t size) { (void)name, (void)is_dir, (void)size; ++*(int *)arg; }
static const char *rom_rel(const char *guest) {   /* the path inside the RomFS mount, or NULL */
    const char *colon = strchr(guest, ':');
    if (!colon || !hos_romfs_dir) return NULL;
    char root[HOS_FS_PATH];
    if (!host_path(guest, root, sizeof root, NULL)) return NULL;
    os_rwlock_lock_shared(&mount_lock);
    int rom = 0;
    for (int i = 0; i < nmounts; i++)
        if (strlen(mounts[i].name) == (size_t)(colon - guest) && !strncmp(mounts[i].name, guest, (size_t)(colon - guest)))
            rom = !mounts[i].writable && !strcmp(mounts[i].root, hos_romfs_dir);
    os_rwlock_unlock_shared(&mount_lock);
    if (!rom) return NULL;
    const char *rest = colon + 1;
    while (*rest == '/') rest++;
    return rest;
}
static int is_mod_dir(const char *guest) {
    const char *rel = rom_rel(guest);
    int n = 0;
    if (rel) hos_mods_dir_extras(rel, "<", count_extra, &n);   /* ("<": no such folder: every entry counts) */
    return n > 0;
}

/* GetEntryType(DirectoryEntryType*, char const*): 0 directory, 1 file */
HLE(_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc) {
    char path[HOS_FS_PATH];
    if (!host_path(gstr(ARG(1)), path, sizeof path, NULL)) { RET(R_PATH_NOT_FOUND); return; }
    int kind = os_path_kind(path);
    if (kind == OS_PATH_NONE && is_mod_dir(gstr(ARG(1)))) kind = OS_PATH_DIR;   /* (only in mods) */
    if (fs_trace())
        hos_log("fs: GetEntryType(%s): %s", gstr(ARG(1)), kind == OS_PATH_NONE ? "not found"
                : (kind & OS_PATH_DIR) ? "directory" : "file");
    if (kind == OS_PATH_NONE) { RET(R_PATH_NOT_FOUND); return; }
    G32(ARG(0)) = (kind & OS_PATH_DIR) ? 0 : 1;
    RET(0);
}

/* ---- directories: DirectoryEntry = { char name[0x301]; pad[3]; s8 type at 0x304 (0 directory, 1 file); pad[3]; s64 size at 0x308 } ---- */
static void add_extra(void *arg, const char *name, int is_dir, uint64_t size) {
    Handle *dh = (Handle *)arg;
    if (!(dh->nextras & 15)) dh->extras = (ExtraEntry *)realloc(dh->extras, sizeof *dh->extras * (size_t)(dh->nextras + 16));
    ExtraEntry *e = &dh->extras[dh->nextras++];
    snprintf(e->name, sizeof e->name, "%s", name);
    e->is_dir = is_dir, e->size = size;
}
HLE(_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci) {
    char path[HOS_FS_PATH];
    if (!host_path(gstr(ARG(1)), path, sizeof path, NULL)) { RET(R_PATH_NOT_FOUND); return; }
    Handle *dh = (Handle *)calloc(1, sizeof *dh);
    dh->is_dir = (int)ARG(2);   /* mode: 1 directories, 2 files, 3 both */
    snprintf(dh->path, sizeof dh->path, "%s", path);   /* (for GetDirectoryEntryCount) */
    dh->dir = os_dir_open(path, NULL);   /* (dh->de: the next entry, not read yet) */
    if (!dh->dir || !os_dir_next(dh->dir, &dh->de)) dh->find_done = 1;
    if (rom_rel(gstr(ARG(1)))) hos_mods_dir_extras(rom_rel(gstr(ARG(1))), path, add_extra, dh);
    if (fs_trace())
        hos_log("fs: OpenDirectory(%s, mode %d)%s, %d entries from mods", gstr(ARG(1)), (int)ARG(2),
                !dh->dir ? ": not found" : "", dh->nextras);
    if (!dh->dir && !dh->nextras) { free(dh); RET(R_PATH_NOT_FOUND); return; }
    G64(ARG(0)) = new_handle(dh);
    RET(0);
}
/* ReadDirectory(long* count, DirectoryEntry* entries, DirectoryHandle, long max) */
HLE(_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl) {
    Handle *dh = get_handle(c, ARG(2));
    int64_t n = 0, max = (int64_t)ARG(3);
    while (n < max && !dh->find_done) {
        const os_dirent *de = &dh->de;
        int isdir = (de->kind & OS_PATH_DIR) != 0;
        if ((isdir && (dh->is_dir & 1)) || (!isdir && (dh->is_dir & 2))) {
            uint64_t e = ARG(1) + 0x310 * n;
            memset(GP(e), 0, 0x310);
            snprintf((char *)GP(e), 0x301, "%s", de->name);
            *(uint8_t *)GP(e + 0x304) = isdir ? 0 : 1;
            G64(e + 0x308) = de->size;
            n++;
        }
        if (!os_dir_next(dh->dir, &dh->de)) dh->find_done = 1;
    }
    while (n < max && dh->find_done && dh->extra_pos < dh->nextras) {   /* then the mods' own entries */
        const ExtraEntry *x = &dh->extras[dh->extra_pos++];
        if (!((x->is_dir && (dh->is_dir & 1)) || (!x->is_dir && (dh->is_dir & 2)))) continue;
        uint64_t e = ARG(1) + 0x310 * n;
        memset(GP(e), 0, 0x310);
        snprintf((char *)GP(e), 0x301, "%s", x->name);
        *(uint8_t *)GP(e + 0x304) = x->is_dir ? 0 : 1;
        G64(e + 0x308) = x->size;
        n++;
    }
    G64(ARG(0)) = (uint64_t)n;
    RET(0);
}
/* (runtime) GetDirectoryEntryCount(long* count, DirectoryHandle): every entry the open mode lists, read or not */
HLE(_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE) {
    Handle *dh = get_handle(c, ARG(1));
    int64_t n = 0;
    os_dir *d = dh->path[0] ? os_dir_open(dh->path, NULL) : NULL;
    os_dirent de;
    while (d && os_dir_next(d, &de)) {
        int isdir = (de.kind & OS_PATH_DIR) != 0;
        n += (isdir && (dh->is_dir & 1)) || (!isdir && (dh->is_dir & 2));
    }
    os_dir_close(d);
    for (int i = 0; i < dh->nextras; i++)
        n += (dh->extras[i].is_dir && (dh->is_dir & 1)) || (!dh->extras[i].is_dir && (dh->is_dir & 2));
    G64(ARG(0)) = (uint64_t)n;
    RET(0);
}
HLE(_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE) {
    Handle *dh = get_handle(c, ARG(0));
    os_dir_close(dh->dir);
    free_handle(ARG(0));
    free(dh->extras);
    free(dh);
}
