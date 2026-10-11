#include <stdlib.h>

/* Free on one branch but reset to NULL, then guard the use. */
int main(int argc, char **argv) {
    (void)argv;
    int *p = malloc(sizeof(int));
    if (p == NULL)
        return 1;
    *p = 1;
    if (argc > 2) {
        free(p);
        p = NULL;
    }
    if (p != NULL) {
        *p = 2;
        free(p);
    }
    return 0;
}
