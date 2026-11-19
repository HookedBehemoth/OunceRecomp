/* (Windows) libxml2's functions as LLVM's manifest merger (lld-link's /manifest:embed with input manifests) calls them.
   The LLVM release's libraries are built with libxml2 but do not ship it; the game's link passes /manifest:no, so
   nothing reaches these. Each stops the process with its name should that change. */
#include <stdio.h>
#include <stdlib.h>

#define STUB(name) void *name(void) { fprintf(stderr, "ounce: lld-link called libxml2's " #name ": manifest merging is not built in\n"); abort(); }

STUB(xmlAddChild)
STUB(xmlCopyNamespace)
STUB(xmlDocDumpFormatMemoryEnc)
STUB(xmlDocGetRootElement)
STUB(xmlDocSetRootElement)
STUB(xmlFreeDoc)
STUB(xmlFreeNode)
STUB(xmlFreeNs)
STUB(xmlNewDoc)
STUB(xmlNewNs)
STUB(xmlNewProp)
STUB(xmlReadMemory)
STUB(xmlStrdup)
STUB(xmlUnlinkNode)
/* (called when the merger is made, before any manifest is read: nothing to do) */
void xmlSetGenericErrorFunc(void *ctx, void *handler) { (void)ctx, (void)handler; }
/* (libxml2's xmlFree is a function pointer variable) */
static void free_stub(void *p) { (void)p; fprintf(stderr, "ounce: lld-link called libxml2's xmlFree\n"); abort(); }
void (*xmlFree)(void *) = free_stub;
