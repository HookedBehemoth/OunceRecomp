/* Horizon OS layer, native host: process start and boot.

   What rtld and nn::init::Start do on hardware (see ports/zelda-totk/PORT.md, "How the game starts"), done here:
   map the guest window, load the relocated image (nx_image.py already linked it), make this host thread the
   guest main thread, run every module's DT_INIT in rtld's order (the SDK crt's _init: TLS registration with musl,
   then the module's constructors), then nninitStartup and nnMain.

   Usage: hookshot [image.bin] [--romfs <dir>] [--user <dir>] [--trace]
   Environment: HOS_TRACE=1 (log HLE calls), HOS_MEMORY_MB (memory the game is told it has),
                HOS_NVN_CONTINUE=1 (go on past nvnBootstrapLoader). */
#include "hos.h"
#include "game_hooks.h"
#include "hos_paths.h"
#include "vk_api.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef HOOKSHOT_VERSION   /* (ounce/builder gives it: the release and a fingerprint of the sources, ounce/builder/src/version.rs) */
#define HOOKSHOT_VERSION "unknown"
#endif
const char *hos_version(void) { return HOOKSHOT_VERSION; }
#include "os.h"
#include "os_guest.h"
#include "hos_sdl.h"
/* (linux) the separator of the paths the executable's folder is joined with */

#ifdef A64_MEM_ZERO
/* web: guest address = wasm address; the guest window is wasm memory below the host's data (-sGLOBAL_BASE) */
extern char __global_base;
uint64_t hos_space_end;
#define GUEST_SPACE (hos_space_end)
#else
#define GUEST_SPACE HOS_GUEST_SPACE
#endif
#define IMAGE_BASE hos_image_base   /* (the lift's: Eden's layout) */


/* gen/hos_image.c (build.py lift) */
extern const uint64_t hos_main_stack_size;
extern const int hos_main_priority, hos_main_core, hos_module_count;
extern const uint64_t hos_module_init[];
extern const char *const hos_module_name[];
extern const uint64_t hos_entry_nninitStartup, hos_entry_nnMain;
extern const uint64_t hos_entry_init_libc0, hos_entry_init_libc1, hos_entry_init_libc2, hos_entry_nnosInitialize,
    hos_entry_nninitInitializeSdkModule;
/* nn_fs.c */
extern const char *hos_romfs_dir, *hos_user_dir;

/* (hos_large_pages: the guest window in locked large pages; never set on Linux) */
int hos_large_pages;   /* (hos_mem.c: freed pages are not discarded then) */

/* The guest window lives at the constant A64_HOST_BASE (rt/a64.h), so lifted code never loads its base. */
static int reserve_guest(void) {
#if defined(A64_MEM_ZERO)
    hos_space_end = (uint64_t)(uintptr_t)&__global_base & ~0xFFFFULL;
    return hos_space_end >= (1ULL << 30);
#else
    void hos_commit_init(void);
    hos_commit_init();   /* (the crash report) */
    /* Shared (a memfd's pages, which the back end imports as dma-bufs instead of host pointers: amdgpu checks every page
       of a host-pointer import again at each submission, 15-45 ms a submission in Kokiri Forest) where the GPU export
       works; HOS_GUEST_MEMFD=1 or 0: shared or private regardless */
    const char *e = getenv("HOS_GUEST_MEMFD"), *note = NULL;
    const int share = !e ? OS_GUEST_SHARE_AUTO : atoi(e) ? OS_GUEST_SHARE_ON : OS_GUEST_SHARE_OFF;
    const int kind = os_guest_reserve((void *)A64_HOST_BASE, GUEST_SPACE, share, &note);
    if (kind < 0) return 0;
    if (kind == OS_GUEST_SHARED) hos_log("mem: the guest window is shared with the GPU as dma-bufs");
    else hos_log("mem: the guest window is private memory, host-pointer imports for the GPU (slow submissions on AMD): %s",
                 note ? note : "-");
    return 1;
#endif
}

/* The guest window, for the renderer (gpu/legacy/render/rx_buffer.cpp). */
uint8_t *hos_guest_base(void) { return A64_MEM; }
uint64_t hos_guest_size(void) { return GUEST_SPACE; }

/* Call guest code at `addr` as a fresh call from the top of the main thread's stack, with whatever arguments the
   caller put in x0-x7 (call_guest clears x0 and x1 first; call_guest_args keeps them). */
static void call_guest_args(Ctx *c, uint64_t addr, uint64_t sp, const char *what) {
    A64Fn f = a64_lookup(addr);
    if (!addr || !f) hos_fatal(c, "%s: no function at %#llx", what, (unsigned long long)addr);
    hos_log("boot: %s (%#llx)", what, (unsigned long long)addr);
    c->x[30] = 0;
    c->x[31] = sp;
    void hos_mem_checkpoint(const char *what);
    hos_mem_checkpoint(what);
    f(c);
}
static void call_guest(Ctx *c, uint64_t addr, uint64_t sp, const char *what) {
    c->x[0] = c->x[1] = 0;
    call_guest_args(c, addr, sp, what);
}

/* A folder tree copied and deleted: the input record's copy of the saves (main) */
static int is_dir(const char *p) {
    return (os_path_kind(p) & OS_PATH_DIR) != 0;
}
static int copy_tree(const char *from, const char *to) {
    if (!is_dir(from)) {
        FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
        int ok = in && out;
        char buf[1 << 16];
        for (size_t n; ok && (n = fread(buf, 1, sizeof buf, in)) > 0;) ok = fwrite(buf, 1, n, out) == n;
        ok = ok && in && !ferror(in);
        if (in) fclose(in);
        if (out && fclose(out)) ok = 0;
        return ok;
    }
    if (os_mkdir(to) && !is_dir(to)) return 0;
    os_dir *d = os_dir_open(from, NULL);
    os_dirent e;
    int ok = 1;
    while (d && ok && os_dir_next(d, &e)) {
        char a[OS_PATH_MAX], b[OS_PATH_MAX];
        snprintf(a, sizeof a, "%s/%s", from, e.name);
        snprintf(b, sizeof b, "%s/%s", to, e.name);
        ok = copy_tree(a, b);
    }
    os_dir_close(d);
    return ok;
}
static void delete_tree(const char *p) {
    if (!is_dir(p)) {
        os_remove(p);
        return;
    }
    os_dir *d = os_dir_open(p, NULL);
    os_dirent e;
    while (d && os_dir_next(d, &e)) {
        char a[OS_PATH_MAX];
        snprintf(a, sizeof a, "%s/%s", p, e.name);
        delete_tree(a);
    }
    os_dir_close(d);
    os_rmdir(p);
}
/* (linux) Saves live in the game folder's user/ when it has one (the builder makes it), so the folder holds the whole
   game; else in $XDG_DATA_HOME/hookshot (~/.local/share/hookshot). --user still wins. */
static const char *saves_dir(const char *beside) {
    static char dir[OS_PATH_MAX];
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && xdg[0] == '/') snprintf(dir, sizeof dir, "%s/hookshot", xdg);
    else if (home && home[0] == '/') snprintf(dir, sizeof dir, "%s/.local/share/hookshot", home);
    else dir[0] = 0;
    if (is_dir(beside)) {
        char old[OS_PATH_MAX + 8], mine[OS_PATH_MAX + 8];
        snprintf(old, sizeof old, "%s/save", dir);
        snprintf(mine, sizeof mine, "%s/save", beside);
        if (dir[0] && is_dir(old) && !is_dir(mine)) hos_log("saves: in %s; an earlier build's are in %s (copy them over to continue them)", beside, dir);
        return beside;
    }
    if (!dir[0]) return beside;
    if (xdg && xdg[0] == '/') os_mkdir(xdg);
    else {
        char p[OS_PATH_MAX];
        snprintf(p, sizeof p, "%s/.local", home), os_mkdir(p);
        snprintf(p, sizeof p, "%s/.local/share", home), os_mkdir(p);
    }
    os_mkdir(dir);
    return os_path_kind(dir) == OS_PATH_NONE ? beside : dir;
}

/* hookshot_env.txt beside the game: NAME=VALUE lines set as environment variables before anything reads them, for the
   switches players are asked to test (RX_VK_NOOPT and the rest). Lines starting with # are notes. On Linux this runs
   before the window, so HOS_HEADLESS and HOS_WINDOW_SIZE are already set when the window is made.
   HOS_ENV_FILE=<path> reads that file instead: a scripted or A/B test run brings its own switches and leaves the
   player's hookshot_env.txt as it is.
   (hookshot) hookshot_env.txt is a file players pass around, so it sets only the game's own switches (RX_, HOS_,
   HOOKSHOT_, EDEN_, NVN_, TOTK_ and SDL_ names), never HOS_ENV_FILE or SDL's library override, and no value that is a
   path: the system's variables (the Vulkan loader's layers, PATH and the like) can load code, and
   the dump switches write files where they name. The file HOS_ENV_FILE names sets anything. */
static int env_line_allowed(const char *name, const char *value) {
    static const char *const prefixes[] = {"RX_", "HOS_", "HOOKSHOT_", "EDEN_", "NVN_", "TOTK_", "SDL_"};
    int ok = 0;
    for (size_t i = 0; i < sizeof prefixes / sizeof *prefixes; i++) ok |= !strncmp(name, prefixes[i], strlen(prefixes[i]));
    if (!ok || !strcmp(name, "HOS_ENV_FILE") || !strcmp(name, "SDL_DYNAMIC_API")) return 0;
    /* (hookshot) nor the libraries SDL loads by name (SDL_VULKAN_LIBRARY, SDL_OPENGL_LIBRARY, SDL_VIDEO_GL_DRIVER,
       SDL_VIDEO_EGL_DRIVER...): a bare file name is looked for beside the game and in the current folder on Windows */
    if (!strncmp(name, "SDL_", 4) && (strstr(name, "_LIB") || strstr(name, "GL_DRIVER"))) return 0;
    for (const char *p = name; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')) return 0;
    /* (hookshot) the switches that name a file to write or a folder take no value here, not even a bare name: one
       resolves in the game's folder (a dump file named hookshot_direct.so emptied the renderer) */
    const size_t nl = strlen(name);
    int names_file = (nl > 5 && (!strcmp(name + nl - 5, "_FILE") || !strcmp(name + nl - 4, "_DIR"))) ||
                     !strcmp(name, "RX_VK_FRAME_LOG") || !strcmp(name, "HOS_PRESENT_TRACE");
    /* (hookshot) and the test switches whose value is a file or folder under another name (HOS_AUDIO_DUMP=image.bin
       overwrote the game's image), or that press buttons and menu keys on a timer (a shared file erased a save from
       the file select) */
    static const char *const by_name[] = {
        "HOS_AUDIO_DUMP", "HOS_AUDIO_WB_DUMP", "HOS_SHADER_DUMP", "HOS_FS_OVERLAY", "HOS_MODS", "HOS_INPUT_LIVE",
        "HOS_INPUT_SCRIPT", "HOS_INPUT_RECORD",
        "HOS_INPUT_REPLAY", "HOOKSHOT_BUILDER_HOME", "RX_VK_SHOTS", "RX_VK_SHOT_GO",
        "RX_VK_FRAMELOG", "RX_VK_HANG_DUMP", "RX_VK_DUMP", "RX_VK_DUMP_SPV", "RX_VK_DUMP_MESH_SPV", "RX_VK_DUMP_STAGES",
        "RX_VK_DUMP_FAILED", "RX_VK_IR", "RX_VK_MENU_SCRIPT", "RX_DUMP_WGSL", "RX_DUMP_SLOW_WGSL", "RX_DUMP_TICS",
        "RX_WGSL_OVERRIDE"};
    for (size_t i = 0; i < sizeof by_name / sizeof *by_name; i++) names_file |= !strcmp(name, by_name[i]);
    if (names_file) return 0;
    return !strchr(value, '/') && value[0] != '~' && strcmp(value, ".") && strcmp(value, "..");
}
static void load_hookshot_env(void) {
    char path[OS_PATH_MAX + 32];
    const char *other = getenv("HOS_ENV_FILE");
    if (other && *other) {
        snprintf(path, sizeof path, "%s", other);
    } else {
        uint32_t n = hos_game_path_a(path, OS_PATH_MAX);
        while (n && path[n - 1] != '/') n--;
        memcpy(path + n, "hookshot_env.txt", 17);
    }
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s += 3;   /* (Notepad's BOM) */
        while (*s == ' ' || *s == 9) s++;
        size_t len = strlen(s);
        while (len && (s[len - 1] == 10 || s[len - 1] == 13 || s[len - 1] == ' ' || s[len - 1] == 9)) s[--len] = 0;
        if (!len || *s == '#' || !strchr(s, '=')) continue;
        char *eq = strchr(s, '=');
        *eq = 0;
        if (!(other && *other) && !env_line_allowed(s, eq + 1)) {
            fprintf(stderr, "env: hookshot_env.txt: %s left out (only the game's switches, and no paths, are taken from it)\n", s);
            continue;
        }
        setenv(s, eq + 1, 1);
    }
    fclose(f);
}

float hos_aspect_ratio = 0;   /* (the window's default shape: runtime/display.c) */
/* (games/oot/patches/oot_probes.c, the widescreen rows) read by the widescreen patches in the game's code; 0: the game's own 16:9 */
volatile uint64_t hos_wide_out, hos_wide_scene;
/* (games/oot/patches/oot_probes.c) mode 1's scene base scale 1.0 instead of 0.8: Resolution 100% (runtime/nn_vi.c keeps it current),
   HOS_SCENE_NATIVE=0/1 over it */
volatile int hos_native_scene;
volatile int hos_fix_res;   /* (games/oot/patches/oot_probes.c's cached-scale refresh: HOS_FIX_RES=1; #849's fixed-scale patch does it by default) */
volatile float hos_wide_ratio, hos_wide_squeeze, hos_wide_margin;
/* (games/oot/patches/oot_probes.c hos_oot_occlusion_flag) read by the game's code where it decides to make occlusion queries: 1, none (HOOKSHOT_OCCLUSION_OFF=1) */
volatile int hos_occlusion_off;
static void occlusion_switch(void) {
    const char *e = getenv("HOOKSHOT_OCCLUSION_OFF");
    hos_occlusion_off = e && e[0] == '1';
    if (hos_occlusion_off) hos_log("occlusion off: the game makes no occlusion queries (HOOKSHOT_OCCLUSION_OFF=1)");
}
/* (hookshot, issue 1241) The game needs about 10 GB of memory and swap together (the page file on Windows): on 8 GB of RAM
   and no swap it closed before the title with no word of why, and adding swap let it play. Below that, the log says so and
   a message says it once at boot; the game starts anyway. The text, or 0 when there is enough */
#define HOS_MEMORY_NEEDED_MB 10240ull
static int memory_notice_text(unsigned long long ram_mb, unsigned long long swap_mb, char *out, size_t cap) {
    if (ram_mb + swap_mb >= HOS_MEMORY_NEEDED_MB) return 0;
    snprintf(out, cap,
             "This PC has %.1f GB of memory and %.1f GB of swap (the page file on Windows). The game needs about %llu GB of the "
             "two together; with less it can close by itself while it loads.\n\nClose other programs, or add swap (on Windows: "
             "let it manage the page file's size), then start the game again. It starts now anyway.",
             ram_mb / 1024.0, swap_mb / 1024.0, HOS_MEMORY_NEEDED_MB / 1024);
    return 1;
}
#ifndef A64_MEM_ZERO
static void memory_notice(void) {
    uint64_t ram_b, swap_b;
    if (os_mem_available(&ram_b, &swap_b)) return;
    const unsigned long long ram = ram_b >> 20, swap = swap_b >> 20;   /* (MB) */
    char text[512];
    if (!memory_notice_text(ram, swap, text, sizeof text)) {
        hos_log("boot: %llu MB of memory, %llu MB of swap", ram, swap);
        return;
    }
    hos_log("boot: LOW MEMORY: %llu MB of memory, %llu MB of swap, below the ~%llu MB the game needs", ram, swap, HOS_MEMORY_NEEDED_MB);
    if (getenv("HOS_HEADLESS")) return;
    hos_sdl_message_box("Hookshot", text);
}
#endif
static int game_main(int argc, char **argv) {
    occlusion_switch();
    {   // (started from its icon the game has nowhere to log to: hookshot_log.txt beside it, last run only, for players to send in)
        if (!os_stderr_shown()) {
            char path[OS_PATH_MAX + 32];
            uint32_t n = hos_game_path_a(path, OS_PATH_MAX);
            while (n && path[n - 1] != '/') n--;
            /* (the four runs before stay as hookshot_log.1.txt (the last) to .4.txt: a player who started the game again after a
               crash sent in the new run's log, and the crash was gone) */
            char from[OS_PATH_MAX + 32], to[OS_PATH_MAX + 32];
            for (int k = 4; k >= 1; k--) {
                if (k == 1) memcpy(from + n, "hookshot_log.txt", 17), memcpy(from, path, n);
                else memcpy(from, path, n), snprintf(from + n, 32, "hookshot_log.%d.txt", k - 1);
                memcpy(to, path, n), snprintf(to + n, 32, "hookshot_log.%d.txt", k);
                os_rename(from, to, true);
            }
            memcpy(path + n, "hookshot_log.txt", 17);
            if (freopen(path, "w", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
        }
    }
    hos_log("hookshot %s", HOOKSHOT_VERSION);   /* (the log's first line: which build a report comes from) */
    const char *image = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--romfs") && i + 1 < argc) hos_romfs_dir = argv[++i];
        else if (!strcmp(argv[i], "--user") && i + 1 < argc) hos_user_dir = argv[++i];
        else if (!strcmp(argv[i], "--trace")) hos_trace = 1;
        else if (!image) image = argv[i];
    }
    static char beside_image[OS_PATH_MAX], beside_romfs[OS_PATH_MAX], beside_user[OS_PATH_MAX];
    if (!image) {   /* (hookshot) started with nothing, as a game is: everything beside the executable (the builder's folder) */
        char dir[OS_PATH_MAX];
        const uint32_t len = hos_game_path_a(dir, OS_PATH_MAX);
        char *slash = len ? strrchr(dir, '/') : NULL;
        if (slash) {
            *slash = 0;
            os_chdir(dir);
            snprintf(beside_image, OS_PATH_MAX, "%s/image.bin", dir);
            snprintf(beside_romfs, OS_PATH_MAX, "%s/romfs", dir);
            snprintf(beside_user, OS_PATH_MAX, "%s/user", dir);
            if (os_path_kind(beside_image) != OS_PATH_NONE) {
                image = beside_image;
                if (!hos_romfs_dir) hos_romfs_dir = beside_romfs;
                if (!hos_user_dir) hos_user_dir = saves_dir(beside_user);
            }
        }
    }
    if (!image) {
        fprintf(stderr, "usage: %s [image.bin] [--romfs <dir>] [--user <dir>] [--trace]\n",
                argv[0]);
        return 2;
    }
    if (getenv("HOS_TRACE")) hos_trace = 1;
    if (!hos_user_dir) hos_user_dir = "user";
    os_mkdir(hos_user_dir);
    {   /* HOS_INPUT_RECORD=<dir>: the save folder as the game starts with it, to <dir>/user (a replay starts from it;
           nn_hid.c writes <dir>/input.rec) */
        const char *rec = getenv("HOS_INPUT_RECORD");
        if (rec && *rec) {
            char dir[OS_PATH_MAX], user[OS_PATH_MAX];
            snprintf(dir, sizeof dir, "%s", rec);
            for (char *p = dir + 1; *p; p++)   /* (each folder of the path) */
                if (*p == '/') { *p = 0; os_mkdir(dir); *p = '/'; }
            os_mkdir(dir);
            snprintf(user, sizeof user, "%s/user", dir);
            delete_tree(user);
            hos_log("hid: input record: save folder %s %s %s", hos_user_dir,
                    copy_tree(hos_user_dir, user) ? "copied to" : "NOT fully copied to", user);
        }
    }
    if (!reserve_guest()) {
        fprintf(stderr, "cannot reserve the guest window\n");
        return 1;
    }
#ifdef A64_MEM_ZERO
    web_fs_init(NULL, hos_romfs_dir);
    /* (the whole file, no range: the server sends it brotli-compressed, 85 -> 26 MB) */
    const int got = web_fetch_all(image, A64_MEM + IMAGE_BASE, GUEST_SPACE - IMAGE_BASE);
    if (got < 0) {
        fprintf(stderr, "cannot fetch %s\n", image);
        return 1;
    }
    size_t n = (size_t)got;
#else
    FILE *f = fopen(image, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", image);
        return 1;
    }
    /* exactly the file's size: a read into "the rest of the guest window" (~4 GB) made Windows commit physical memory
       for ~2 GB of it before the game ran (the whole heap was resident from the start, ~1.3 GB of it never used) */
    fseek(f, 0, SEEK_END);
    const long long file_size = ftello(f);
    fseek(f, 0, SEEK_SET);
    if (file_size > 0) hos_commit(IMAGE_BASE, (uint64_t)file_size);
    size_t n = file_size > 0 && (uint64_t)file_size <= GUEST_SPACE - IMAGE_BASE
                   ? fread(A64_MEM + IMAGE_BASE, 1, (size_t)file_size, f) : 0;
    fclose(f);
#endif
    if (IMAGE_BASE + n != hos_image_end) {
        fprintf(stderr, "%s: %zu bytes, the lifted code expects %#llx: lift and image do not match\n", image, n,
                (unsigned long long)(hos_image_end - IMAGE_BASE));
        return 1;
    }
    hos_mem_init(hos_image_end);
    void hos_code_ptrs_init(void);
    hos_code_ptrs_init();   /* code_ptrs.c */
    {   /* (hookshot) The game's frame follows the window (games/oot/patches/oot_probes.c's rows): its pixels are the window's (at most
           3840 x 2160's count, the shape kept) and the main camera, HUD and movies take its shape; a resize, once settled,
           sizes the frame again (runtime/media/display_sdl.c). The game draws its frame into a corner of its picture (the window
           textures), so the picture is made once as large as any frame: the monitor's size, at least 3840 x 2160. With
           HOS_ASPECT=<ratio> or an explicit Output resolution (HOS_OUTPUT_RES, output_res) the frame is the game's instead:
           its picture takes that shape (or the window's) at the output's height, and a resize letterboxes it.
           Wider than 16:9 the HUD, menus and movies keep the middle 16:9; narrower they fill the frame's width. */
        const char *e = getenv("HOS_ASPECT");
        int ww = 0, wh = 0, dw = 0, dh = 0;
        hos_screen_size(&dw, &dh);   /* (0 x 0 headless) */
        if (getenv("HOS_WINDOW_SIZE") && sscanf(getenv("HOS_WINDOW_SIZE"), "%dx%d", &ww, &wh) == 2 && ww >= 64 && wh >= 64) {
        } else if (dh > 0) {
            wh = 720, ww = (int)(720.0 * dw / dh + 0.5);   /* (the window display_sdl.c opens) */
        } else {
            ww = 1920, wh = 1080;   /* (the headless image's) */
        }
        float want = e ? (float)atof(e) : (float)ww / (float)wh;
        if (getenv("HOS_SCENE_NATIVE")) hos_native_scene = atoi(getenv("HOS_SCENE_NATIVE")) != 0;
        if (getenv("HOS_FIX_RES")) hos_fix_res = atoi(getenv("HOS_FIX_RES")) != 0;
        if (hos_native_scene) hos_log("boot: HOS_SCENE_NATIVE=1: the scene's base scale 1.0 (the game's 0.8 otherwise)");
        extern volatile uint64_t hos_wide_out, hos_wide_scene;
        extern volatile float hos_wide_ratio, hos_wide_squeeze, hos_wide_margin;
        int hos_output_res_set(void);   /* (runtime/nn_applet.c: an Output resolution was chosen) */
        const double game = 16.0 / 9.0;
        if (!e && !hos_output_res_set()) {
            const unsigned pw = (unsigned)(dw > 3840 ? dw : 3840), ph = (unsigned)(dh > 2160 ? dh : 2160);
            hos_wide_out = (uint64_t)ph << 32 | pw;
            hos_wide_scene = (uint64_t)(ph * 3 / 4) << 32 | (pw * 3 / 4);
            hos_aspect_ratio = want;
            game_frame_size((uint32_t)ww, (uint32_t)wh, 1);
            hos_log("boot: the frame follows the window: picture %ux%u", pw, ph);
        } else if (want > game * 1.01 || (want > 1.0f && want < game * 0.99)) {
            /* (a screen narrower than 16:9 too, 16:10 to 5:4: the picture keeps the game's height and is narrower, the
               camera takes its ratio, and the HUD fills its width instead of a middle 16:9 wider than the screen. A 4:3
               CRT at the 1440 output: picture 1920 x 1440, scene 1440 x 1080, HUD 100%) */
            double r = want > 32.0 / 9.0 ? 32.0 / 9.0 : want < 5.0 / 4.0 ? 5.0 / 4.0 : want;
            uint32_t hos_output_size(int height);   /* (runtime/nn_applet.c: the Output resolution's height, 1440 as shipped) */
            unsigned oh = hos_output_size(1), ow = (unsigned)(oh * r / 8 + 0.5) * 8;   /* (whole multiples of 8 pixels) */
            /* (at most 3840 x 2160's pixels, the shape kept: the game's render targets come from a pool sized for its picture
               (runtime/nn_applet.c hos_rt_pool_size), and a 32:9 screen at the 4K output made a 7680 x 2160 one. Its pool at the
               output's size ran out as Kokiri Forest loaded (a null pointer, issue 1031); a pool for the whole picture, 1.4 GB,
               failed the game's own reservation at boot. 5400 x 1520 there, more than the 5120 x 1440 screens it was seen on) */
            if ((double)ow * oh > 3840.0 * 2160.0) {
                oh = (unsigned)(sqrt(3840.0 * 2160.0 / r) / 8) * 8;
                ow = (unsigned)(oh * r / 8) * 8;
            }
            const unsigned sh = oh * 3 / 4, sw = (unsigned)(ow * 0.75 / 8 + 0.5) * 8;   /* (the scene, 3/4 of the picture as shipped) */
            hos_wide_out = (uint64_t)oh << 32 | ow;
            hos_wide_scene = (uint64_t)sh << 32 | sw;
            hos_wide_ratio = (float)ow / (float)oh;
            hos_wide_squeeze = (float)fmin(1.0, game / ((double)ow / oh));   /* (a narrower screen: the whole width) */
            hos_wide_margin = (1.0f - hos_wide_squeeze) / 2.0f;
            hos_aspect_ratio = hos_wide_ratio;
            hos_log("boot: widescreen: picture %ux%u, scene %ux%u, camera ratio %.4f, HUD and movies in the middle %.1f%%", ow, oh, sw, sh,
                    (double)hos_wide_ratio, hos_wide_squeeze * 100.0);
        } else if (want > 1.0f) {
            hos_log("boot: widescreen: a %.4f screen is 16:9: the game's own picture", (double)want);
        }
    }
    hos_log("boot: image %zu MB at %#llx, %llu MB of memory for the game", n >> 20, IMAGE_BASE,
            (unsigned long long)(hos_total_memory >> 20));
#ifndef A64_MEM_ZERO
    memory_notice();
#endif
    void hos_mods_init(const char *image_path);
    hos_mods_init(image);   /* mods.c: <exe folder>\mods (RomFS files) */

    Ctx *c = hos_boot_main_thread(hos_main_stack_size);
    uint64_t sp = c->x[31];
    void hos_display_start(void);
    hos_display_start();  /* the game's window; HOS_HEADLESS=1 for none */
    /* nn::init::Start's order (sdk+0xfa060), with rtld's three callbacks done here: f1 only marks rtld initialized,
       f2 is every module's DT_INIT, f3 the DT_FINIs (not needed: we never return from nnMain to unload). */
    call_guest(c, hos_entry_init_libc0, sp, "_init_libc0");
    c->x[0] = hos_thread_handle(hos_self());   /* the main thread's handle */
    c->x[1] = 0;                               /* program arguments: none (an unmapped address) */
    call_guest_args(c, hos_entry_nnosInitialize, sp, "nnosInitialize");
    void hos_thread_adopt_guest(HosThread *t);
    hos_thread_adopt_guest(hos_self());
    call_guest(c, hos_entry_init_libc1, sp, "_init_libc1");
    /* nninitInitializeSdkModule (sdk+0xfa000) only opens the SDK's service clients (sm:, diag, oe, aoc, err, fs,
       pctl) over IPC; the HLE stands in for every one of them, so it is not run. */
    call_guest(c, hos_entry_nninitStartup, sp, "nninitStartup");
    call_guest(c, hos_entry_init_libc2, sp, "_init_libc2");
    for (int i = 0; i < hos_module_count; i++) {
        char what[64];
        snprintf(what, sizeof what, "%s DT_INIT", hos_module_name[i]);
        call_guest(c, hos_module_init[i], sp, what);
    }
    call_guest(c, hos_entry_nnMain, sp, "nnMain");
    hos_log("nnMain returned");
    return 0;
}

/* (linux) The window belongs on the process's main thread (SDL), so the game runs on a thread of its own, with the
   stack the guest main thread's lifted code needs (the main thread has the system's 8 MB): runtime/media/display_sdl.c */
static int game_argc;
static char **game_argv;
static void game_thread(void *p) {
    (void)p;
    os_exit(game_main(game_argc, game_argv));
}
int main(int argc, char **argv) {
    int hos_display_init(void);
    void hos_display_loop(void);
    load_hookshot_env();
    game_argc = argc, game_argv = argv;
    const int window = hos_display_init();
    if (window < 0) {
        hos_log_flush();
        return 1;
    }
    os_thread *t = os_thread_start(game_thread, NULL, 64u << 20, "Game");
    if (!t) {
        fprintf(stderr, "cannot start the game's thread\n");
        return 1;
    }
    if (window) hos_display_loop();   /* (does not return: the window's events until the process ends) */
    os_thread_join(t, OS_FOREVER);
    return 0;
}
