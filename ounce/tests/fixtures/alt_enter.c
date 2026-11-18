#include <stdio.h>
#include <stdlib.h>
#define VK_RETURN 0x0D
#define VK_MENU 0x12
#define VK_TAB 0x09
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
static int held[256];
static int down(int vk) { return held[vk & 255]; }
@KMAP_DOWN@
int main(void) {
    held[VK_RETURN] = 1;
    CHECK(kmap_down(VK_RETURN, down));            /* Enter alone is Plus */
    held[VK_MENU] = 1;
    CHECK(!kmap_down(VK_RETURN, down));           /* Alt+Enter is not */
    held[VK_TAB] = 1;
    CHECK(kmap_down(VK_TAB, down));               /* Alt does not hide the other keys */
    held[VK_RETURN] = 0;
    CHECK(!kmap_down(VK_RETURN, down));
    held[VK_MENU] = 0;
    CHECK(!kmap_down(VK_MENU, down) && kmap_down(VK_TAB, down));
    printf("PASS: Alt+Enter is fullscreen, not Plus\n");
    return 0;
}
