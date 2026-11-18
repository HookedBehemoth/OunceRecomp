#define _GNU_SOURCE
#include "os.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define HOS_FS_PATH 1024   /* (runtime/nn_fs.c's path buffers, #1497) */
static void hos_log(const char *f, ...) { (void)f; }
static void lower(char *s) { (void)s; }
static int nput;
static void put(const char *key, const char *host, uint64_t size, int is_dir) { (void)key; (void)host; (void)size; (void)is_dir; nput++; }
@PIECES@
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    CHECK(guest_path_ok("") && guest_path_ok("slot_00/progress.sav") && guest_path_ok("a//b/c.d"));
    CHECK(!guest_path_ok("..") && !guest_path_ok("slot_00/../../x") && !guest_path_ok("a/./b"));
    CHECK(!guest_path_ok("a\nb"));   /* ('\\' and ':' are plain characters: '/' is the only separator) */
    char save[PATH_MAX], victim[PATH_MAX], keep[PATH_MAX], cmd[3 * PATH_MAX];
    snprintf(save, sizeof save, "%s/save", argv[1]);
    snprintf(victim, sizeof victim, "%s/victim", argv[1]);
    snprintf(keep, sizeof keep, "%s/victim/keep.txt", argv[1]);
    snprintf(cmd, sizeof cmd, "mkdir -p '%s/slot_00/inner' '%s' && echo x > '%s' && echo s > '%s/slot_00/progress.sav' && "
             "ln -s '%s' '%s/slot_00/link' && ln -s '%s' '%s/slot_00/inner/file'", save, victim, keep, save, victim, save, keep, save);
    CHECK(system(cmd) == 0);
    char slot[PATH_MAX];
    snprintf(slot, sizeof slot, "%s\\slot_00", save);
    delete_tree(slot);
    struct stat st;
    CHECK(stat(keep, &st) == 0);      /* what the links pointed to is still there */
    CHECK(lstat(slot, &st) != 0);     /* and the slot is gone, links and all */
    char romfs[PATH_MAX];
    snprintf(romfs, sizeof romfs, "%s/romfs", argv[1]);
    snprintf(cmd, sizeof cmd, "mkdir -p '%s/UI' && echo m > '%s/UI/a.bin' && ln -s . '%s/up1' && ln -s . '%s/up2' && "
             "ln -s '%s' '%s/UI/b.bin'", romfs, romfs, romfs, romfs, keep, romfs);
    CHECK(system(cmd) == 0);
    const int files = index_romfs(romfs, "", 0);
    CHECK(files == 2);                /* a.bin and the file link b.bin; not the folder links */
    char linked_root[PATH_MAX];       /* a mod whose romfs folder itself is a link */
    snprintf(linked_root, sizeof linked_root, "%s/linked-romfs", argv[1]);
    snprintf(cmd, sizeof cmd, "ln -s '%s' '%s'", romfs, linked_root);
    CHECK(system(cmd) == 0);
    CHECK(index_romfs(linked_root, "", 0) == 0);
    char dirlink[PATH_MAX];
    snprintf(dirlink, sizeof dirlink, "%s/up1", romfs);
    CHECK(os_path_kind(dirlink) == (OS_PATH_LINK | OS_PATH_DIR));
    CHECK(os_path_kind(romfs) == OS_PATH_DIR);
    printf("PASS: guest paths stay in their mounts; deleting a save folder removes links, not their targets; mods skip folder links\n");
    return 0;
}
