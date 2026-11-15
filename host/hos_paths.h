/* The game folder's path, for the files the game keeps there (image.bin, romfs, saves, settings, caches, logs).
   On Windows the game is bin\hookshot.exe since the launcher came: hookshot.exe in the folder is the launcher (the builder's
   setup in launch mode), the game and its DLLs are in bin\, the game's files stay in the folder. hos_game_path_a/_w answer as
   GetModuleFileName(NULL, ...) does, but with the folder's path when the exe is in a "bin" folder beside image.bin, so code that
   strips the exe's name and adds a file's finds the folder's file. Modules (DLLs) are still loaded from beside the exe itself. */
#pragma once
#include <string.h>
#include <wchar.h>
#include <unistd.h>
#include <windows.h>   /* (linux: host/posix's, for DWORD and GetModuleFileNameA) */
/* (Linux) the same bin/ layout when the folder was built by the packaged builder: hookshot in the folder is the
   launcher script, and the game is bin/hookshot. A development build has no bin/ and this is GetModuleFileName. */
static inline DWORD hos_game_path_a(char *buf, DWORD size) {
    DWORD n = GetModuleFileNameA(NULL, buf, size);
    if (!n || n >= size) return n;
    char *name = buf + n;
    while (name > buf && name[-1] != '/') name--;
    if (name - buf < 5) return n;
    char *dir = name - 1;
    char *bin = dir;
    while (bin > buf && bin[-1] != '/') bin--;
    if (dir - bin != 3 || memcmp(bin, "bin", 3) != 0) return n;
    char probe[MAX_PATH + 16];
    const size_t root = (size_t)(bin - buf);
    if (root + 10 >= sizeof probe) return n;
    memcpy(probe, buf, root);
    memcpy(probe + root, "image.bin", 10);
    if (access(probe, F_OK) != 0) return n;
    memmove(buf + root, name, strlen(name) + 1);
    return (DWORD)strlen(buf);
}
