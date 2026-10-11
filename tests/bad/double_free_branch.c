#include <stdlib.h>

/* Error path frees, then falls through to the common cleanup. */
int process(int fail) {
    int *data = malloc(10 * sizeof(int));
    if (data == NULL)
        return -1;
    data[0] = 42;
    if (fail) {
        free(data);     /* freed only on this path ... */
    }
    free(data);         /* ... so this may be a double free */
    return 0;
}

int main(int argc, char **argv) {
    (void)argv;
    return process(argc > 1);
}
