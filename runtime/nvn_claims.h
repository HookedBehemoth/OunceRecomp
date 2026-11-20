/* Pool claims: a game patch names what a memory pool the game makes next holds, before its nvnMemoryPoolInitialize.
   The back end serves only claimed kinds of memory the console's unified memory gave for free; an NVN memory call no
   claim covers is an error (runtime/nvn_native.c). A claim covers every pool the claiming thread makes between begin and
   end, so one patch on an engine allocator claims all of its pools. */
#pragma once

enum {
    NVN_CLAIM_NONE = 0,
    /* render-target storage: virtual and physical pools whose textures' images the GPU writes; nothing reads or
       writes their guest bytes, so they have no memory behind their GPU range (MapVirtual is accepted, nothing done) */
    NVN_CLAIM_TARGETS = 1,
};

/* by: a fixed string naming the patch, for the log */
void nvn_claim_begin(int kind, const char *by);
void nvn_claim_end(void);
