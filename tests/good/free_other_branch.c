#include <stdlib.h>

/* Freed on one branch, used only on the OTHER branch, never after the join. */
int main(int argc, char **argv) {
    (void)argv;
    int *v = malloc(sizeof(int));
    if (v == NULL)
        return 1;
    if (argc > 1) {
        free(v);
        return 0;
    } else {
        *v = 5;
    }
    int r = *v;                 /* only reached on the "else" path */
    free(v);
    return r;
}
