#include <stdlib.h>

/* After free, p is pointed at a NEW allocation. Using it is fine.
   In SSA form the two mallocs are different values, so this is not flagged. */
int main(void) {
    char *p = malloc(16);
    if (p == NULL)
        return 1;
    p[0] = 'a';
    free(p);
    p = malloc(16);
    if (p == NULL)
        return 1;
    p[0] = 'b';
    free(p);
    return 0;
}
