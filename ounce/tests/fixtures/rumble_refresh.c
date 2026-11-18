#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d %s\n", __LINE__, #x); return 1; } } while (0)
@DUE@
int main(void) {
    CHECK(rumble_due(1000, 0, 0, 0, 1000, 0) == 1);                 /* a controller that has not been sent this yet */
    CHECK(rumble_due(1000, 0, 1000, 0, 1000, 1000) == 0);           /* the same strength, just sent */
    CHECK(rumble_due(1000, 0, 1000, 0, 1000 + @HALF@LL - 1, 1000) == 0);
    CHECK(rumble_due(1000, 0, 1000, 0, 1000 + @HALF@LL, 1000) == 1);    /* still wanted, and the effect would be ending */
    CHECK(rumble_due(0, 0, 0, 0, 1000 + @HALF@LL, 1000) == 0);          /* silence is not sent again */
    CHECK(rumble_due(0, 0, 1000, 200, 1000, 0) == 1);               /* a stop */
    return 0;
}
