#include <stdlib.h>

int main(void) {
    char *p = malloc(32);
    if (p == NULL)
        return 1;
    p[0] = 'a';
    free(p);
    free(p);            /* double free */
    return 0;
}
