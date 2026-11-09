#include <stdlib.h>

/* realloc may move the block. After a successful call the OLD pointer is
   invalid (CERT MEM30-C); only the returned pointer may be used. */
int main(void) {
    char *p = malloc(8);
    if (p == NULL)
        return 1;
    char *q = realloc(p, 16);
    if (q == NULL) {
        free(p);
        return 1;
    }
    p[0] = 'x';   /* use of the old pointer after realloc */
    free(q);
    return 0;
}
