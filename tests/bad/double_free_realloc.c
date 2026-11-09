#include <stdlib.h>

/* realloc already released the old block, so freeing it again is a
   double free. */
int main(void) {
    char *p = malloc(8);
    if (p == NULL)
        return 1;
    char *q = realloc(p, 16);
    if (q == NULL)
        return 1;
    free(p);      /* double free: realloc released p */
    free(q);
    return 0;
}
