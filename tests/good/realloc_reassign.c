#include <stdlib.h>

/* Two correct realloc patterns:
   1. p = realloc(p, n): in SSA form the result is a NEW value, so later
      uses of p are uses of the live block.
   2. tmp = realloc(p, n) with a NULL check: on failure the old block is
      untouched, so freeing p there is correct. */
int main(void) {
    char *p = malloc(8);
    if (p == NULL)
        return 1;
    p = realloc(p, 16);
    if (p == NULL)
        return 1;
    p[15] = 'a';

    char *tmp = realloc(p, 32);
    if (tmp == NULL) {
        free(p);   /* realloc failed: p is still valid */
        return 1;
    }
    p = tmp;
    p[31] = 'b';
    free(p);
    return 0;
}
