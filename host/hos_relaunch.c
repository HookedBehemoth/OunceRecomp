/* (hookshot) The Insert menu's Restart now (gpu/legacy/vk/vk_menu.cpp): it saves its options and asks the window to close for a
   restart (HOS_WINDOW_RESTART to hos_posix_window_ask, host/posix/display_sdl.c). The window closes as when the player closes it, and at
   the very end of that, instead of only exiting, the game is started again:
     Linux: the process becomes the game again (execve) with the arguments and the environment it started with
     (/proc/self/cmdline and /proc/self/environ: hookshot_env.txt's lines are read again, not kept), so whatever waits
     for it (the launcher, a stream, a test) still has it. The game is bin/hookshot on disk, never run from inside the
     AppImage (hookshot-launch starts it with the AppImage's variables taken away), so /proc/self/exe is the game;
     argv[0] on the PATH if that is gone (the folder built again meanwhile). */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"

static volatile int32_t relaunch_asked;

void hos_relaunch_ask(void) { os_atomic_xchg(&relaunch_asked, 1); }

#include "hos_relaunch.h"
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

extern char **environ;

static void relaunch(void) {
    HosRelaunch r;
    if (!hos_relaunch_command("/proc/self/cmdline", "/proc/self/environ", &r)) {
        hos_log("restart: this process's command was not found: the game only exits");
        hos_log_flush();
        return;
    }
    /* (every file this process opened closes with it, as at an exit: the new one opens its own) */
    DIR *d = opendir("/proc/self/fd");
    if (d) {
        const int skip = dirfd(d);
        for (struct dirent *e; (e = readdir(d));) {
            const int fd = atoi(e->d_name);
            if (fd > 2 && fd != skip) fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
        }
        closedir(d);
    }
    fflush(NULL);
    execve("/proc/self/exe", r.argv, r.envp);
    environ = r.envp;   /* (execvp searches the PATH of the environment it is given: the one the game started with) */
    execvp(r.argv[0], r.argv);
    hos_log("restart: the game could not be started again: it only exits");
    hos_log_flush();
}

/* The window was closed (host/posix/display_sdl.c: its close or quit event, the menu's Quit): the game exits, and starts again
   first when the menu asked for it */
void hos_window_closed_exit(void) {
    const int again = relaunch_asked != 0;
    hos_log(again ? "display: window closed for a restart (the Insert menu's Restart now)" : "display: window closed, exiting");
    hos_log_flush();
    if (again) relaunch();
    os_exit(0);
}
